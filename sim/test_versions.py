import json
import os
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

work = tempfile.mkdtemp(prefix="meshcore-ota-test-")
with open(os.path.join(work, "versions.json"), "w") as fh:
    json.dump({"tags": sorted(tags.items())}, fh)
check(mcota_build.releases(work, fresh=False) == [("v1.9.1", a[:7]), ("v1.18.0", c[:7])], "versions from the cache are wrong or in the wrong order")
check(mcota_build.releases(work) == [("v1.9.1", a[:7]), ("v1.18.0", c[:7])], "a recent cache is not reused")

with open(os.path.join(work, "versions.json"), "w") as fh:
    json.dump([{"name": "repeater-v1.17.1", "commit": {"sha": b}}, {"name": "room-server-v1.17.1", "commit": {"sha": a}}], fh)
check(mcota_build.releases(work, fresh=False) == [("v1.17.1", b[:7])], "the cache format of release 1.0.0 is not read")

empty = tempfile.mkdtemp(prefix="meshcore-ota-test-")
check(mcota_build.releases(empty, fresh=False) == [] and "no versions found" in mcota_build.releases_problem(), "an empty cache is not reported")

print(f"Version list: {failures} errors")
sys.exit(1 if failures else 0)
