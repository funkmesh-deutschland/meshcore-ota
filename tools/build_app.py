import os
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sep = ";" if os.name == "nt" else ":"
data = []
for source, target in (("core", "sources/core"), ("firmware/mcota", "sources/firmware/mcota"), ("firmware/boards", "sources/firmware/boards"), ("firmware/esp32", "sources/firmware/esp32"),
                       ("firmware/meshcore.patch", "sources/firmware"), ("loader/nrf52", "sources/loader/nrf52")):
    data += ["--add-data", os.path.join(root, *source.split("/")) + sep + target]
command = [sys.executable, "-m", "PyInstaller", "-y", "--onefile", "--name", "meshcore-ota", "--paths", os.path.join(root, "tools"),
           "--hidden-import", "mcota_companion", "--hidden-import", "mcota_provider", "--hidden-import", "mcota_ui", "--hidden-import", "mcota_build", "--hidden-import", "mcota_menu", "--hidden-import", "mcota_update", "--hidden-import", "mcota_version",
           "--collect-all", "bleak"] + data + [os.path.join(root, "tools", "mcota.py")]
sys.exit(subprocess.call(command))
