#!/usr/bin/env python3
"""Run the game-free host check set.

These checks compile synthetic tests with the host compiler and need no Android
NDK, device, game files, keys or network. Each check writes only below build/.
Checks whose tools are missing are reported as skipped, never as passed.
"""

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT = "ports/shared-android/assets/fonts/shared_5.ttf"
# (command, required executables). Default invocations only: no --android,
# --capture or --original* flags, which need private inputs or an NDK build.
CHECKS = [
    (["tools/check_button_font.py", FONT], []),
    (["tools/check_compatibility.py"], []),
    (["tools/check_compiler_constants_staging.py"], ["clang"]),
    (["tools/check_constant_buffer.py"], ["clang"]),
    (["tools/check_command_lifetime.py"], ["clang"]),
    (["tools/check_compute_headstart.py"], ["clang"]),
    (["tools/check_driver_probe_recovery.py"], ["clang"]),
    (["tools/check_audren_work_size.py"], ["clang"]),
    (["tools/check_audio_splitter.py"], ["clang"]),
    (["tools/check_kernel_readable_copy.py"], ["clang"]),
    (["tools/check_kernel_thread_affinity.py"], ["clang"]),
    (["tools/check_kernel_thread_core_integration.py"], ["clang"]),
    (["tools/check_kernel_thread_start.py"], ["clang"]),
    (["tools/check_kernel_get_info.py"], ["clang"]),
    (["tools/check_headstart_progress.py"], ["clang"]),
    (["tools/check_preparation_shutdown.py"], ["clang"]),
    (["tools/check_startup_prep_ui.py"], ["clang++"]),
    (["tools/check_shader_cache_identity.py"], ["clang", "clang++"]),
    (["tools/check_shader_discovery.py"], ["clang", "clang++"]),
    (["tools/check_shader_discovery_runtime.py"], ["clang", "clang++"]),
    (["tools/check_shader_worker_policy.py"], ["clang"]),
    (["tools/check_shader_compiler.py"], ["clang"]),
    (["tools/check_direct_spirv_integration.py"], ["clang"]),
    (["tools/check_direct_spirv.py", "--no-gpu"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_shader_worker_queue.py"], ["clang"]),
    (["tools/check_shader_job_failures.py"], ["clang"]),
    (["tools/check_shader_source_index.py"], ["clang"]),
    (["tools/check_shader_profile.py"], ["clang", "clang++"]),
    (["tools/check_shader_profile_index.py"], ["clang", "clang++"]),
    (["tools/check_shader_profile_catalog.py"], ["clang", "clang++"]),
    (["tools/check_import_shader_variants.py"], ["clang", "clang++"]),
    (["tools/check_graphics_pipeline_library.py"], ["clang"]),
    (["tools/check_offline_services.py"], ["clang"]),
    (["tools/check_npad_history.py"], ["clang"]),
    (["tools/check_nvn_proc_diagnostics.py"], ["clang"]),
    (["tools/check_sampled_texture_handle.py"], ["clang"]),
    (["tools/check_shared_hash.py"], ["clang++"]),
    (["tools/check_shared_input_policy.py"], ["clang"]),
    (["tools/check_shared_present_pacing.py"], ["clang"]),
    (["tools/check_arbiter_trace.py"], ["clang"]),
    (["tools/check_bound_projection.py"], ["clang"]),
    (["tools/check_graphics_ready.py"], ["clang"]),
    (["tools/check_pass_gpu.py"], ["clang"]),
    (["tools/check_imgui_vk_failfast.py"], ["clang++"]),
    (["tools/check_mirror_capacity.py"], ["clang"]),
    (["tools/check_mirror_normalize.py"], ["clang"]),
    (["tools/check_storage_image_backend.py"], ["clang"]),
    (["tools/check_uniform_forward.py"], ["clang"]),
    (["tools/check_gcopy_probe.py"], ["clang"]),
    (["tools/check_clip_distance.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_bindless_tex.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_echoes_shader_ops.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_echoes_shaders.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_headstart_external_constants.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_mixed_half_shader.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_local_memory_shader.py"], ["clang", "clang++", "glslangValidator", "spirv-val"]),
    (["tools/check_shader_metadata.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_texture_gather.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_vmnmx_atom_shader.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_shared_writable_shaders.py"], ["clang", "glslangValidator", "spirv-val"]),
    (["tools/check_generic_adapter.py"], ["clang", "clang++"]),
    (["tools/check_native_session_prepare.py"], ["clang", "clang++"]),
    (["tools/check_raitchi_shell_bridge.py"], ["javac", "java"]),
    (["tools/check_raitchi_ui_lifecycle.py"], ["javac", "java"]),
    (["tools/check_research_environment.py"], ["javac", "java"]),
    (["tests/test_button_font.py"], []),
    (["tests/shared_parallel_build_test.py"], []),
    (["tests/stage_shared_manual_test.py"], []),
]

# These read the staged Android tree (work/shared-android), Vulkan headers from
# the dependency build or build/shared-android/native/compile_commands.json, so
# they run with --android after `build_shared_android.py --build`.
ANDROID_CHECKS = [
    "tools/check_gpu_cache_compat.py", "tools/check_graphics_mirror.py",
    "tools/check_nvn_texture_view.py", "tools/check_pool_mapping.py",
    "tools/check_stencil_sampler.py", "tools/check_material_attributes.py",
    "tools/check_material_projection.py", "tools/check_sampling_trace.py",
    "tools/check_texture_write.py", "tools/check_renderer_policy.py",
    "tools/check_shader_snapshot.py", "tools/check_physical_memory.py",
    "tools/check_command_pool_stats.py", "tools/check_sampled_float_compatible.py",
    "tools/check_texture_3d_attachments.py",
    "tools/check_clear_scope_probe.py", "tools/check_ldc_signed_offsets.py",
    "tools/check_linear_texture_layout.py", "tools/check_projection_texels.py",
    "tools/check_nvn_command_shadow.py",
]

ANDROID_COMPILE_FLAGS = {
    "tools/check_graphics_mirror.py", "tools/check_stencil_sampler.py",
    "tools/check_material_probe.py", "tools/check_nvn_command_shadow.py",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-k", "--filter", help="only run checks whose command contains this text")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--android", action="store_true",
                    help="run the checks that need a completed native Android build instead")
    ap.add_argument("--report", type=Path, default=ROOT / "build" / "host-checks.json")
    args = ap.parse_args()
    results = []
    checks = [([c, *(["--android"] if c in ANDROID_COMPILE_FLAGS else [])], ["clang"])
              for c in ANDROID_CHECKS + ["tools/check_material_probe.py"]] \
        if args.android else CHECKS
    if args.android and not (ROOT / "build/shared-android/native/compile_commands.json").is_file():
        ap.error("--android needs a completed `tools/build_shared_android.py --build`")
    for cmd, needs in checks:
        name = " ".join(cmd)
        if args.filter and args.filter not in name:
            continue
        missing = [n for n in needs if not shutil.which(n)]
        if cmd[0].startswith(("tools/", "tests/")) and not (ROOT / cmd[0]).exists():
            missing.append(cmd[0])
        if missing:
            results.append({"check": name, "status": "skipped", "missing": missing})
            print(f"SKIP {name} (missing {', '.join(missing)})", flush=True)
            continue
        start = time.monotonic()
        try:
            p = subprocess.run([sys.executable, *cmd], cwd=ROOT, capture_output=True,
                               text=True, timeout=args.timeout)
            status = "passed" if p.returncode == 0 else "failed"
            tail = (p.stdout + p.stderr)[-1500:]
        except subprocess.TimeoutExpired:
            status, tail = "failed", "timeout"
        secs = round(time.monotonic() - start, 1)
        results.append({"check": name, "status": status, "seconds": secs,
                        **({"output_tail": tail} if status == "failed" else {})})
        print(f"{status.upper():6} {name} ({secs}s)", flush=True)
        if status == "failed":
            print("  " + tail.strip().replace("\n", "\n  ")[-800:], flush=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(results, indent=1) + "\n", encoding="utf-8")
    counts = {s: sum(r["status"] == s for r in results) for s in ("passed", "failed", "skipped")}
    print(f"host checks: {counts['passed']} passed, {counts['failed']} failed, "
          f"{counts['skipped']} skipped")
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
