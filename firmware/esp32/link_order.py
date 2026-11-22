import os
Import("env")

def reorder(text):
    out, done, held = [], 0, None
    for line in text.split("\n"):
        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]
        if stripped.startswith("*(EXCLUDE_FILE(") and any(key in line for key in (" .text ", " .rodata ", " .data ", " .bss ", " COMMON)")):
            out.append(indent + "*.a:" + stripped)
            done += 1
            if " .text " in line:
                held = line
                continue
        if held is not None and stripped.startswith("*(.stub "):
            out.append(indent + ". = ALIGN(4096);")
            out.append(indent + "_mcota_app_start = ABSOLUTE(.);")
            out.append(held)
            held = None
        out.append(line)
    return "\n".join(out), done

source = None
for folder in env.get("LIBPATH", []):
    candidate = os.path.join(env.subst(str(folder)), "sections.ld")
    if os.path.exists(candidate):
        source = candidate
if source:
    target_dir = os.path.join(env.subst("$BUILD_DIR"), "mcota_ld")
    os.makedirs(target_dir, exist_ok=True)
    text, done = reorder(open(source).read())
    open(os.path.join(target_dir, "sections.ld"), "w").write(text)
    env.Prepend(LIBPATH=[target_dir])
    print("MeshCore-OTA: fixed order, %d places in %s" % (done, source))
