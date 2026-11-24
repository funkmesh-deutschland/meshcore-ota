import sys
import time
from collections import deque

from mcota_provider import CHUNK
from rich import box
from rich.align import Align
from rich.console import Console, Group
from rich.live import Live
from rich.panel import Panel
from rich.progress_bar import ProgressBar
from rich.table import Table
from rich.text import Text

TEAL = "#0f766e"
DARK = "#0b2424"
ORANGE = "#fc790d"
PAPER = "#ffffff"
SOFT = "#5f7472"
RAIL = "#cfe3e0"
RED = "#b91c1c"

ACCENT = TEAL
GOOD = TEAL
WARN = ORANGE
BAD = RED
DIM = SOFT
BASE = f"{DARK} on {PAPER}"
BLANK = box.Box("    \n    \n    \n    \n    \n    \n    \n    \n")

console = Console()


def fancy():
    return sys.stdout.isatty()


def banner(subtitle="Firmware updates for MeshCore repeaters over LoRa"):
    word = Text(style=f"on {PAPER}")
    word.append("FUNKMESH\n", style=f"bold {TEAL}")
    word.append("DEUTSCHLAND e.V.\n", style=f"bold {DARK}")
    word.append("funkmesh.de", style=SOFT)
    title = Text(justify="right", style=f"on {PAPER}")
    title.append("MeshCore", style=f"bold {DARK}")
    title.append("-OTA\n", style=f"bold {TEAL}")
    title.append(subtitle + "\n", style=SOFT)
    title.append("Your key, your repeaters", style=ORANGE)
    line = Table.grid(expand=True, padding=(0, 2))
    line.add_column(ratio=1)
    line.add_column(justify="right")
    line.add_row(word, title)
    return Panel(line, box=box.HEAVY, border_style=TEAL, style=BASE, padding=(0, 2))


def show_banner(subtitle=None):
    if fancy():
        console.print(banner(subtitle) if subtitle else banner())


def table(title, columns):
    grid = Table(title=None, box=box.SIMPLE_HEAD, expand=True, header_style=f"bold {TEAL}", border_style=RAIL, pad_edge=False, style=BASE)
    for name, kwargs in columns:
        grid.add_column(name, **kwargs)
    return grid


def panel(body, title):
    return Panel(body, title=f"[bold {TEAL}]{title}[/]", title_align="left", border_style=RAIL, box=box.ROUNDED, padding=(0, 1), style=BASE)


def bar(done, total, width=None, color=TEAL):
    return ProgressBar(total=max(total, 1), completed=min(done, total), width=width, complete_style=color, finished_style=TEAL, style=RAIL)


def age(seconds):
    seconds = int(seconds)
    if seconds < 90:
        return f"{seconds} s ago"
    if seconds < 5400:
        return f"{seconds // 60} min ago"
    return f"{seconds // 3600} h ago"


def span(seconds):
    seconds = int(seconds + 0.5)
    if seconds < 3600:
        return f"{seconds // 60}:{seconds % 60:02d} min"
    return f"{seconds // 3600} h {seconds % 3600 // 60:02d} min"


def version_of(manifest):
    return manifest[36:52].split(b"\0")[0].decode(errors="replace")


def board_of(manifest):
    return manifest[20:36].split(b"\0")[0].decode(errors="replace")


class Dashboard:
    def __init__(self, ppm, test):
        self.ppm = ppm
        self.test = test
        self.names = {}
        self.events = deque(maxlen=7)
        self.seen = {}
        self.started = time.monotonic()
        self.companion = None
        self.providers = []
        self.target = None
        self.live = Live(self.render(), console=console, screen=True, refresh_per_second=4, transient=False)

    def note(self, text, style=DARK):
        self.events.appendleft((time.strftime("%H:%M:%S"), text, style))

    def name(self, key):
        return self.names.get(key, "")

    def current(self):
        latest = {}
        for provider in self.providers:
            for key, peer in provider.peers.items():
                if key not in latest or peer["at"] > latest[key][1]["at"]:
                    latest[key] = (provider, peer)
        return latest

    def progress(self, provider, key, peer):
        if peer["kind"] == 0:
            total = (len(provider.manifest) + CHUNK - 1) // CHUNK
            left = sum(bin(entry[2]).count("1") for entry in provider.repairs if entry[0] == 0)
            return "manifest", total - min(left, total), total
        changed = sum(1 for block in provider.blocks if block)
        left = len(provider.wants.get(bytes.fromhex(key), ()))
        left += sum(1 for entry in provider.repairs if entry[0] == 1)
        return "pages", changed - min(left, changed), changed

    def packets(self, provider, key):
        total = provider.packets_total()
        left = min(total, provider.packets_left(bytes.fromhex(key)))
        return total - left, total, left

    def next_packet(self, provider):
        wait = provider.next_in()
        if wait is None and not provider.busy():
            return None
        return "next packet now" if wait is None or wait < 1.0 else f"next packet in {span(wait)}"

    def track(self):
        for key, (provider, peer) in self.current().items():
            version = version_of(provider.manifest)
            stage, done, total = self.progress(provider, key, peer)
            state = (version, stage, done >= total)
            before = self.seen.get(key)
            label = self.name(key) or key
            if before is None or before[0] != version:
                self.note(f"{label} loading {version}", TEAL)
            elif before[1] == "manifest" and stage == "pages":
                self.note(f"{label} has verified the manifest for {version} and requests pages")
            elif stage == "pages" and not before[2] and state[2]:
                self.note(f"{label}: all {total} changed pages for {version} are sent, the rest stays the same", TEAL)
            self.seen[key] = state

    def update(self, companion, providers):
        self.companion = companion
        self.providers = providers
        self.track()
        self.live.update(self.render())

    def companion_panel(self):
        c = self.companion
        grid = Table.grid(padding=(0, 2))
        grid.add_column(style=SOFT)
        grid.add_column()
        if c is None:
            grid.add_row("Connection", Text("connecting ...", style=ORANGE))
        else:
            grid.add_row("Name", Text(c.name, style="bold"))
            grid.add_row("ID", c.public_key[:4].hex())
            grid.add_row("Radio", f"{c.freq / 1000:.3f} MHz · {c.bw / 1000:g} kHz · SF{c.sf} · CR 4/{c.cr}")
            grid.add_row("Power", f"{c.tx_power} dBm")
            grid.add_row("Firmware", c.firmware)
            grid.add_row("Target", Text(self.target or "matching repeaters in range and up to 2 hops", style=TEAL))
        return panel(grid, "Companion")

    def airtime_panel(self):
        grid = Table.grid(padding=(0, 2))
        grid.add_column(style=SOFT)
        grid.add_column()
        limit = self.ppm / 10000
        grid.add_row("Allowed", Text(f"send at most {limit:g} % of the time" + (" (test mode)" if self.test else ""), style=f"bold {TEAL}"))
        if self.providers or (self.companion and getattr(self.companion, "finder", None)):
            budget = self.providers[0].budget if self.providers else self.companion.finder.budget
            sent = sum(p.sent for p in self.providers)
            sent_ms = sum(p.sent_ms for p in self.providers)
            elapsed = max(time.monotonic() - self.started, 1.0)
            used = sent_ms / 10.0 / elapsed
            grid.add_row("Actual", Text(f"{used:.2f} % since start", style=TEAL if used <= limit * 1.05 else ORANGE))
            grid.add_row("Budget", Text(f"{budget.ms / 1000:.1f} s saved of at most 10 s", style=DARK))
            grid.add_row("", bar(budget.ms, 10000.0, 30))
            grid.add_row("Sent", f"{sent} packets · {sent_ms / 1000:.0f} s airtime")
            waits = [p.next_in() for p in self.providers if p.next_in() is not None]
            if waits:
                grid.add_row("Next packet", Text("now" if min(waits) < 1.0 else f"in {span(min(waits))}", style=TEAL))
            elif any(p.busy() for p in self.providers):
                grid.add_row("Next packet", Text("now", style=TEAL))
            else:
                grid.add_row("Next packet", Text("nothing waiting", style=DARK))
            grid.add_row("Requests", f"{sum(p.requests for p in self.providers)} from repeaters")
        minutes = int((time.monotonic() - self.started) // 60)
        grid.add_row("Running", f"{minutes // 60} h {minutes % 60:02d} min")
        return panel(grid, "Airtime")

    def packages_panel(self):
        grid = table("Packages", [("Device", {"style": "bold"}), ("from", {}), ("to", {"style": f"bold {TEAL}"}), ("to transfer", {}),
                                ("Radio packets", {"justify": "right"}), ("Apply", {}), ("loading now", {"justify": "right"})])
        loading = {}
        for key, (provider, _) in self.current().items():
            loading[id(provider)] = loading.get(id(provider), 0) + 1
        for provider in self.providers:
            m = provider.manifest
            changed = sum(1 for block in provider.blocks if block)
            packets = (len(m) + CHUNK - 1) // CHUNK + provider.packets_total()
            grid.add_row(board_of(m), getattr(provider, "old_version", "") or m[84:88].hex(), version_of(m), f"{changed} changed pages, {provider.pages - changed} stay the same", str(packets),
                         Text("automatic", style=TEAL) if m[5] & 1 else Text("on command", style=ORANGE), str(loading.get(id(provider), 0) or ""))
        return panel(grid, "Offered updates")

    def peers_panel(self):
        grid = table("Repeaters", [("Repeater", {"style": "bold"}), ("loading", {}), ("Status", {"ratio": 1}),
                                  ("Signal", {"justify": "right"}), ("last request", {"justify": "right", "style": SOFT})])
        rows = 0
        loading = self.current()
        finder = getattr(self.companion, "finder", None) if self.companion else None
        for key, hello in sorted(finder.seen.items()) if finder else []:
            if key in loading:
                continue
            label = f"{self.name(key)} ({key})" if self.name(key) else key
            state = self.companion.states.get(key, "")
            rssi = hello.get("rssi")
            signal = Text(f"{hello['hops']} hops" if hello.get("hops", 0) > 1 else "1 hop", style=SOFT) if hello.get("hops") else (Text(f"{rssi} dBm", style=TEAL if rssi is not None and rssi > -100 else ORANGE) if rssi is not None else Text(""))
            grid.add_row(label, hello["tag"] or "?", Text(state, style=TEAL if state.startswith("is up to date") else ORANGE), signal, age(finder.now() - hello["at"]))
            rows += 1
        for key, (provider, peer) in sorted(loading.items()):
            label = f"{self.name(key)} ({key})" if self.name(key) else key
            stage, done, total = self.progress(provider, key, peer)
            upcoming = self.next_packet(provider)
            detail = None
            if stage == "manifest":
                status = Text(f"manifest, packet {done} of {total} sent", style=ORANGE)
                upcoming = Text(upcoming, style=TEAL) if upcoming else Text("waiting for the repeater", style=SOFT)
            else:
                sent, packets, left = self.packets(provider, key)
                now = provider.current()
                if left == 0:
                    status = Text(f"all {packets} packets sent, {total} changed pages" if packets != 1 else "the one packet sent", style=f"bold {TEAL}")
                    upcoming = Text("waiting for the repeater", style=SOFT)
                else:
                    status = Text(f"{sent} of {packets} packets sent ({100 * sent // max(packets, 1)} %), {left} to go")
                    if now and now[0] == 1 and now[3] > 1:
                        detail = Text(f"page {min(done + 1, total)} of {total}: packet {now[2]} of {now[3]} sent", style=SOFT)
                    else:
                        detail = Text(f"{done} of {total} changed pages sent", style=SOFT)
                    upcoming = Text(upcoming or "next packet now", style=TEAL)
                    upcoming.append(f", about {span(provider.time_left(bytes.fromhex(key)))} left", style=SOFT)
                done, total = sent, packets
            cell = Table.grid(expand=True)
            cell.add_column(ratio=1)
            cell.add_row(status)
            cell.add_row(bar(done, total, None, ORANGE if stage == "manifest" else TEAL))
            for line in (detail, upcoming):
                if line:
                    cell.add_row(line)
            status = cell
            rssi = peer.get("rssi")
            signal = Text(f"{len(provider.hops)} hop" + ("s" if len(provider.hops) > 1 else ""), style=SOFT) if provider.hops else (Text(f"{rssi} dBm", style=TEAL if rssi is not None and rssi > -100 else ORANGE) if rssi is not None else Text(""))
            grid.add_row(label, version_of(provider.manifest), status, signal, age(provider.now() - peer["at"]))
            rows += 1
        if rows == 0:
            return panel(Align.center(Text("No repeater has reported yet. The query goes out at growing intervals.", style=SOFT)), "Repeaters")
        return panel(grid, "Repeaters")

    def events_panel(self):
        lines = Table.grid(padding=(0, 1))
        lines.add_column(style=SOFT)
        lines.add_column()
        for stamp, text, style in self.events:
            lines.add_row(stamp, Text(text, style=style))
        if not self.events:
            lines.add_row("", Text("nothing yet", style=SOFT))
        return panel(lines, "Log")

    def render(self):
        top = Table.grid(expand=True)
        top.add_column(ratio=1)
        top.add_column(ratio=1)
        top.add_row(self.companion_panel(), self.airtime_panel())
        foot = Text("Ctrl+C quits. After a restart of the app the repeaters continue where they left off.", style=SOFT, justify="center")
        body = Group(banner(), top, self.packages_panel(), self.peers_panel(), self.events_panel(), foot)
        return Panel(body, box=BLANK, style=BASE, padding=0, height=console.size.height)
