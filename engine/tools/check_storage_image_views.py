#!/usr/bin/env python3
"""Exercise actual storage-view/cache policy with controlled Vulkan calls; compile the full Android TU."""
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def function(text, name):
    match = re.search(r"(?m)^(?:static )?[^;\n]*\b" + re.escape(name) + r"\(", text)
    if not match:
        raise RuntimeError("Missing function " + name)
    start = match.start()
    end = text.index("{", match.end()) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

def struct(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start) + len(last)]

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    out = ROOT / "build/storage-image-views"
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / "ports/shared-android/runtime/nvn/vulkan/textures.c"
    header = source.with_name("vk.h")
    nvn = source.parent.parent / "nvn.h"
    test = ROOT / "tests/storage_image_views_test.c"
    paths = [source, header, nvn, test, Path(__file__)]
    pinned = {str(p.relative_to(ROOT)): sha(p) for p in paths}
    text = source.read_text()
    types = "typedef void *VmaAllocation; typedef struct { unsigned unused; } TexLayout; typedef struct NvnPool NvnPool;\n"
    types += struct(nvn.read_text(), "typedef struct NvnTexture {", "} NvnTexture;") + "\n"
    view = nvn.read_text(); start = view.index("typedef struct {\n    uint32_t setmask;")
    types += view[start:view.index("} NvnTextureView;", start) + len("} NvnTextureView;")] + "\n"
    types += struct(header.read_text(), "typedef struct TexBackend {", "} TexBackend;") + "\n"
    (out / "production_types.inc").write_text(types)
    helpers = "\n".join(function(text, name) for name in ("fmt_of", "renderer_format_has_stencil", "format_supports", "vk_storage_format_kind", "storage_nvn_format", "storage_format_writable", "vk_view_format"))
    (out / "production_formats.inc").write_text("typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;\n" + helpers + "\n")
    view_start = text.index("#define VIEW_TAB ")
    view_end = text.index("// render: attachment view", view_start)
    (out / "production_views.inc").write_text(text[view_start:view_end])
    start = text.index("    bool depthfmt = renderer_format_is_depth(t->format);", text.index("static void create_image("))
    end = text.index("    VkImageCreateFlags flags =", start)
    (out / "production_usage.inc").write_text(text[start:end] + "return usage;\n")
    (out / "production_allocation.inc").write_text(function(text, "create_image_allocation") + "\n")
    assert "b->usage = ci.usage;" in text
    includes = ROOT / "work/las-baseline/build/android/deps/include"
    def run(cmd, log):
        p = subprocess.run(list(map(str, cmd)), cwd=ROOT, capture_output=True, text=True, timeout=60)
        (out / log).write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(log + ": " + p.stdout + p.stderr)
        return p.stdout.strip()
    reports = {}
    cc = shutil.which("clang")
    for mode, flags in (("optimized", ["-O2"]), ("sanitized", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"])):
        binary = out / mode
        run([cc, "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", *flags, "-I", out, "-I", includes, test, "-o", binary], mode + "-build.log")
        reports[mode] = json.loads(run([binary], mode + "-run.log"))
        assert reports[mode]["state"] == "passed"
    rows = json.loads((ROOT / "build/shared-android/native/compile_commands.json").read_text())
    row = next(r for r in rows if r["file"].endswith("/nvn/vulkan/textures.c"))
    virtual_header = str(Path(row["file"]).with_name("vk.h"))
    overlay = out / "overlay.json"
    overlay.write_text(json.dumps({"version": 0, "use-external-names": False, "roots": [
        {"type": "file", "name": row["file"], "external-contents": str(source)},
        {"type": "file", "name": virtual_header, "external-contents": str(header)}]}))
    cmd = shlex.split(row["command"]); cmd[cmd.index("-o") + 1] = str(out / "textures-android.o")
    cmd[1:1] = ["-ivfsoverlay", str(overlay)]
    run(cmd, "android-full-tu.log")
    assert pinned == {str(p.relative_to(ROOT)): sha(p) for p in paths}, "Sources changed during checks"
    report = {"schema": 1, "scope": "Actual storage-view, format, usage/cache C with exact field declarations and controlled Vulkan/CPU-upload stand-ins; full production textures.c Android compile. No actual GPU/image-store/device qualification.", "tests": reports, "android_full_tu": True, "source_sha256": pinned}
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
