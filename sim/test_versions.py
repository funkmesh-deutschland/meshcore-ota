import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import mcota_build

failures = 0


def check(ok, text):
    global failures
    if not ok:
        failures += 1
        print("ERROR:", text)


def line(text):
    return "%04x%s" % (len(text) + 4, text)


a, b, c = "a" * 40, "b" * 40, "c" * 40
refs = (line("# service=git-upload-pack\n") + "0000" + line(a + " HEAD\x00multi_ack side-band-64k\n") + line(a + " refs/heads/main\n")
        + line(b + " refs/tags/repeater-v1.18.0\n") + line(c + " refs/tags/repeater-v1.18.0^{}\n") + line(a + " refs/tags/repeater-v1.9.1\n")
        + line(b + " refs/tags/companion-v1.18.0\n") + "0000")
tags = dict(mcota_build.parse_refs(refs))
check(tags == {"repeater-v1.18.0": c, "repeater-v1.9.1": a, "companion-v1.18.0": b}, f"tags parsed wrongly: {tags}")

def data_dir():
    home = tempfile.mkdtemp(prefix="meshcore-ota-test-")
    os.makedirs(os.path.join(home, "cache"))
    return home, os.path.join(home, "cache")


home, work = data_dir()
with open(os.path.join(work, "versions.json"), "w") as fh:
    json.dump({"tags": sorted(tags.items())}, fh)
check(mcota_build.releases(work, fresh=False) == [("v1.9.1", a[:7]), ("v1.18.0", c[:7])], "versions from the cache are wrong or in the wrong order")
check(mcota_build.releases(work) == [("v1.9.1", a[:7]), ("v1.18.0", c[:7])], "a recent cache is not reused")

with open(os.path.join(work, "versions.json"), "w") as fh:
    json.dump([{"name": "repeater-v1.17.1", "commit": {"sha": b}}, {"name": "room-server-v1.17.1", "commit": {"sha": a}}], fh)
check(mcota_build.releases(work, fresh=False) == [("v1.17.1", b[:7])], "the cache format of release 1.0.0 is not read")

home, work = data_dir()
check(mcota_build.releases(work, fresh=False) == [] and "no versions found" in mcota_build.releases_problem(), "an empty cache is not reported")

before = mcota_build.load_settings(home)["revision"]
local = os.path.join(home, "sources", "v9.9.9")
os.makedirs(local)
os.makedirs(os.path.join(home, "sources", "notes"))
with open(os.path.join(local, "platformio.ini"), "w") as fh:
    fh.write("[platformio]\n")
mcota_build.local_cache.clear()
check(mcota_build.releases(work, fresh=False) == [("v9.9.9", "local-v9.9.9")], "a local source folder is not listed as a version")
check(mcota_build.fetch_source("local-v9.9.9", work) == local, "a local source folder is not used for the build")
check(mcota_build.load_settings(home)["revision"] == before + 1, "a new local source does not raise the settings revision")

for bad in ({"source": "-oProxyCommand=x", "flags": {}}, {"source": "a b", "flags": {}}, {"source": "o/n", "flags": {"X": "-DOK=1 ;rm"}}, {"source": "o/n", "flags": {"X": "--std=c99"}}):
    try:
        mcota_build.check_custom(bad)
        check(False, f"invalid custom settings accepted: {bad}")
    except SystemExit:
        pass
mcota_build.check_custom({"source": "git@example.org:team/firmware.git", "flags": {"X": "-DENV_INCLUDE_BME280=1 -UFOO"}})

if shutil.which("git"):
    origin = tempfile.mkdtemp(prefix="meshcore-ota-test-")
    run = lambda *args: subprocess.run(["git", "-c", "user.name=test", "-c", "user.email=test@example.org", "-c", "init.defaultBranch=main"] + list(args),
                                       cwd=origin, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("init", "-q")
    for tag, text in (("repeater-v2.0.0", "one"), ("v2.1.0", "two")):
        with open(os.path.join(origin, "platformio.ini"), "w") as fh:
            fh.write(text)
        run("add", "-A")
        run("commit", "-q", "-m", tag)
        run("tag", "-a", tag, "-m", tag)
    home, work = data_dir()
    mcota_build.save_custom(home, {"source": origin, "flags": {}})
    found = mcota_build.releases(work)
    check([v for v, _ in found] == ["v2.0.0", "v2.1.0"], f"versions of a git source are wrong: {found} {mcota_build.releases_error}")
    if len(found) == 2:
        tree = mcota_build.fetch_source(found[0][1], work)
        check(open(os.path.join(tree, "platformio.ini")).read() == "one" and not os.path.exists(os.path.join(tree, ".git")), "the source of a git tag was not fetched")
    home, work = data_dir()
    mcota_build.save_custom(home, {"source": os.path.join(origin, "missing"), "flags": {}})
    check(mcota_build.releases(work) == [] and mcota_build.releases_error, "an unreachable git source is not reported")

print(f"Version list: {failures} errors")
sys.exit(1 if failures else 0)
