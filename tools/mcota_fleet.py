import hashlib
import time

from mcota_provider import CHUNK, HELLO_CARRYING, HELLO_NEEDS_COMMAND, NODE_COMPLETE, NODE_FAILED, NODE_FETCHING

LOADER_ARMED, LOADER_TRIAL, LOADER_CONFIRMED, LOADER_REVERTED, LOADER_REJECTED = 1, 2, 3, 4, 5


def text_of(raw):
    return raw.split(b"\0")[0].decode(errors="replace")


def span(seconds):
    seconds = int(seconds + 0.5)
    if seconds < 3600:
        return f"{seconds // 60}:{seconds % 60:02d} min"
    return f"{seconds // 3600} h {seconds % 3600 // 60:02d} min"


def packages_of(providers, old_versions):
    out, seen = [], set()
    for provider in providers:
        ident = provider.campaign.hex()
        if ident in seen:
            continue
        seen.add(ident)
        manifest = provider.manifest
        changed = sum(1 for block in provider.blocks if block)
        out.append({"id": ident[:8], "board": text_of(manifest[20:36]), "from": old_versions.get(provider.campaign, "") or manifest[84:88].hex(),
                    "to": text_of(manifest[36:52]), "changed": changed, "pages": provider.pages,
                    "packets": (len(manifest) + CHUNK - 1) // CHUNK + provider.packets_total(), "apply": "automatic" if manifest[5] & 1 else "on command"})
    return out


def transfer(provider, ident):
    peer = provider.peers.get(ident)
    if peer is None:
        return None
    key = bytes.fromhex(ident)
    if peer["kind"] == 0:
        total = (len(provider.manifest) + CHUNK - 1) // CHUNK
        left = sum(bin(entry[2]).count("1") for entry in provider.repairs if entry[0] == 0)
        return {"stage": "manifest", "done": total - min(left, total), "total": total, "left": min(left, total), "detail": "", "next_s": provider.next_in(), "left_s": None,
                "package": provider.campaign.hex()[:8], "at": peer["at"], "rssi": peer.get("rssi"), "snr": peer.get("snr")}
    total = provider.packets_total()
    left = min(total, provider.packets_left(key))
    changed = sum(1 for block in provider.blocks if block)
    pages_left = min(changed, len(provider.wants.get(key, ())) + sum(1 for entry in provider.repairs if entry[0] == 1))
    now = provider.current()
    if left and now and now[0] == 1 and now[3] > 1:
        detail = f"page {min(changed - pages_left + 1, changed)} of {changed}: packet {now[2]} of {now[3]} sent"
    else:
        detail = f"{changed - pages_left} of {changed} changed pages sent"
    return {"stage": "pages", "done": total - left, "total": total, "left": left, "detail": detail, "next_s": provider.next_in() if left else None,
            "left_s": provider.time_left(key) if left else 0.0, "package": provider.campaign.hex()[:8], "at": peer["at"], "rssi": peer.get("rssi"), "snr": peer.get("snr")}


def judge(hello, state_text, moving):
    node, loader = hello.get("node"), hello.get("loader")
    if loader == LOADER_TRIAL:
        return "trial", "runs the new firmware on trial and waits for an answer from a neighbour"
    if loader == LOADER_ARMED:
        return "applying", "is swapping the firmware"
    if moving and moving["stage"] == "manifest":
        return "loading", f"manifest, packet {moving['done']} of {moving['total']} sent"
    if moving and moving["left"]:
        return "loading", f"{moving['done']} of {moving['total']} packets sent ({100 * moving['done'] // max(moving['total'], 1)} %), {moving['left']} to go"
    if node == NODE_COMPLETE:
        if hello.get("flags", 0) & HELLO_NEEDS_COMMAND:
            return "complete", "has the update completely and waits for the command ota apply"
        wait = hello.get("apply_in")
        if wait is None:
            return "complete", "has the update completely"
        left = max(0.0, wait - (hello["now"] - hello["at"]))
        return "complete", f"has the update completely, applies it in about {span(left)}" if left > 30 else "has the update completely, applies it now"
    if node == NODE_FETCHING:
        pages, have = hello.get("pages") or 0, hello.get("have") or 0
        if hello.get("flags", 0) & HELLO_CARRYING:
            return "carrying", f"carries a package for other repeaters, {have} of {pages} pages"
        source = f" from {hello['from_name']}" if hello.get("from") else ""
        return "loading", f"loading{source}, {have} of {pages} pages" if pages else f"loading{source}"
    if moving and not moving["left"]:
        return "waiting", f"all {moving['total']} packets sent, waiting for the repeater"
    if loader == LOADER_REVERTED:
        return "rolled_back", state_text or "rolled back to the previous firmware"
    if loader == LOADER_REJECTED:
        return "rejected", state_text or "the loader rejected the update"
    if node == NODE_FAILED:
        return "rolled_back", state_text or "does not take this update again"
    if state_text.startswith("is up to date"):
        return "up_to_date", state_text
    if state_text.startswith("gets ") or state_text.startswith("building") or state_text == "checking":
        return "waiting", state_text
    return "unknown", state_text


def hop_ids(path, hop_size, keys):
    out = []
    for pos in range(0, len(path), hop_size):
        prefix = path[pos:pos + hop_size]
        match = [ident for ident, key in keys.items() if key[:hop_size] == prefix]
        out.append(match[0] if len(match) == 1 else "?" + prefix.hex())
    return out


def snapshot(companion, providers, old_versions, saved, log, ppm, test, started, version=""):
    out = {"time": int(time.time()), "version": version, "test": bool(test), "companion": None, "airtime": None, "settings": None,
           "packages": packages_of(providers, old_versions), "repeaters": [], "links": [], "log": list(log)}
    if companion is None:
        for ident, entry in sorted(saved.items()):
            out["repeaters"].append({"id": ident, "name": entry.get("name", ""), "mine": True, "board": entry.get("board", ""), "firmware": entry.get("firmware", ""),
                                     "state": "unknown", "text": "", "lat": entry.get("lat"), "lon": entry.get("lon"), "seen_s": None, "progress": None, "via": [],
                                     "hops": None, "from": "", "level": None, "config": None, "apply_in_s": None, "rssi": None, "snr": None, "stored": [], "peers": []})
        return out
    me = companion.public_key[:4].hex()
    finder, budget = companion.finder, companion.budget
    out["companion"] = {"id": me, "name": companion.name, "lat": companion.lat, "lon": companion.lon, "power": companion.tx_power, "firmware": companion.firmware,
                        "radio": f"{companion.freq / 1000:.3f} MHz · {companion.bw / 1000:g} kHz · SF{companion.sf} · CR 4/{companion.cr}"}
    sent = sum(p.sent for p in providers) + (finder.sent if finder else 0) + (companion.settings.sent if companion.settings else 0)
    sent_ms = sum(p.sent_ms for p in providers) + (finder.sent_ms if finder else 0) + (companion.settings.sent_ms if companion.settings else 0)
    elapsed = max(time.monotonic() - started, 1.0)
    waits = [p.next_in() for p in providers if p.next_in() is not None]
    out["airtime"] = {"base": ppm / 10000, "now": (budget.ppm if budget else ppm) / 10000, "max": (budget.most if budget else ppm) / 10000,
                      "used": sent_ms / 10.0 / elapsed, "heard": (budget.heard_ms if budget else 0.0) / 10.0 / elapsed,
                      "budget_s": (budget.ms if budget else 0.0) / 1000, "sent": sent, "sent_s": sent_ms / 1000,
                      "next_s": min(waits) if waits else (0.0 if any(p.busy() for p in providers) else None), "running_s": elapsed,
                      "requests": sum(p.requests for p in providers)}
    if companion.settings:
        out["settings"] = {"revision": companion.settings.revision, "bytes": len(companion.settings.blob), "packets": companion.settings.count}
    names = dict(companion.names)
    names[me] = companion.name
    for ident, entry in saved.items():
        if entry.get("name"):
            names.setdefault(ident, entry["name"])
    seen = dict(finder.seen) if finder else {}
    idents = set(seen) | set(saved) | set(companion.absent)
    for provider in providers:
        idents |= set(provider.peers)
    links = {}

    def link(source, target, kind, **more):
        if source and target and source != target:
            links[(source, target, kind)] = dict({"from": source, "to": target, "kind": kind}, **more)

    for ident in sorted(idents):
        hello = dict(seen.get(ident) or {})
        entry = saved.get(ident, {})
        moving = None
        for provider in providers:
            found = transfer(provider, ident)
            if found and (moving is None or found["at"] > moving["at"]):
                moving = found
        hello["now"] = finder.now() if finder else time.monotonic()
        hello["from_name"] = names.get(hello.get("from", ""), hello.get("from", ""))
        state, text = judge(hello, companion.states.get(ident, ""), moving) if (hello.get("id") or moving) else ("unknown", companion.states.get(ident, ""))
        if not hello.get("id") and not moving:
            state = "unreachable" if ident in companion.absent else "unknown"
        route = hello.get("route") or (finder.routes.get(ident) if finder else None)
        via = hop_ids(route["path"], route["hop_size"], companion.keys) if route else []
        position = (entry.get("lat"), entry.get("lon")) if entry.get("lat") is not None else companion.positions.get(ident, (None, None))
        progress = None
        if moving:
            progress = {"done": moving["done"], "total": moving["total"], "unit": "packets", "detail": moving["detail"], "next_s": moving["next_s"], "left_s": moving["left_s"]}
        elif hello.get("node") == NODE_FETCHING and hello.get("pages"):
            progress = {"done": hello["have"], "total": hello["pages"], "unit": "pages", "detail": "", "next_s": None, "left_s": None}
        apply_in = None
        if hello.get("node") == NODE_COMPLETE and hello.get("apply_in") is not None:
            apply_in = max(0.0, hello["apply_in"] - (hello["now"] - hello["at"]))
        heard = hello.get("at")
        if moving and (heard is None or moving["at"] > heard):
            heard = moving["at"]
        out["repeaters"].append({
            "id": ident, "name": names.get(ident, ""), "mine": ident in saved, "board": entry.get("board", ""), "firmware": hello.get("tag") or entry.get("firmware", ""),
            "state": state, "text": text, "progress": progress, "from": hello.get("from") or (me if moving and moving["left"] else ""), "via": via,
            "hops": (route["hops"] if route else 0) if (hello.get("id") or moving) else None,
            "level": hello["level"] / 10000 if hello.get("level") else None, "config": hello.get("config"), "apply_in_s": apply_in,
            "rssi": hello.get("rssi") if hello.get("rssi") is not None else (moving or {}).get("rssi"), "snr": hello.get("snr") if hello.get("snr") is not None else (moving or {}).get("snr"),
            "seen_s": hello["now"] - heard if heard is not None else None, "last_seen": entry.get("seen"), "lat": position[0], "lon": position[1],
            "stored": [item.hex() for item in hello.get("stored", [])], "peers": [{"id": peer, "missing": missing} for peer, missing in hello.get("peers", [])]})
        if route:
            chain = [me] + via + [ident]
            for a, b in zip(chain, chain[1:]):
                link(a, b, "route")
        if moving:
            link(me, ident, "data", package=moving["package"], progress=moving["done"] / max(moving["total"], 1), active=bool(moving["left"]))
        if hello.get("from") and hello.get("node") == NODE_FETCHING and not (moving and hello["from"] == me):
            link(hello["from"], ident, "data", package=hello.get("campaign", b"").hex(), progress=(hello.get("have") or 0) / max(hello.get("pages") or 1, 1), active=True)
        for peer, missing in hello.get("peers", []):
            link(ident, peer, "data", package="", progress=None, active=missing > 0, missing=missing)
    watcher = companion.watcher
    for target, entry in (watcher.links.items() if watcher else []):
        if (entry["from"], target, "data") in links or time.monotonic() - entry["at"] > 3600:
            continue
        pages, missing = entry.get("pages"), entry.get("missing")
        link(entry["from"], target, "data", package=entry["campaign"].hex(), progress=(1 - missing / pages) if pages and missing is not None else None, active=True, missing=missing)
    known = {repeater["id"] for repeater in out["repeaters"]} | {me}
    for item in links.values():
        for end in (item["from"], item["to"]):
            if end not in known:
                known.add(end)
                position = companion.positions.get(end, (None, None))
                out["repeaters"].append({"id": end, "name": names.get(end, ""), "mine": False, "board": "", "firmware": "", "state": "relay" if not end.startswith("?") else "hop",
                                         "text": "", "progress": None, "from": "", "via": [], "hops": None, "level": None, "config": None, "apply_in_s": None, "rssi": None,
                                         "snr": None, "seen_s": None, "last_seen": None, "lat": position[0], "lon": position[1], "stored": [], "peers": []})
    out["links"] = list(links.values())
    return out
