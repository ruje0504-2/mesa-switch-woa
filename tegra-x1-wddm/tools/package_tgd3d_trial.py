#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package tgd3d with a reviewed .158 KMD and manual recovery tools."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def identity(path):
    return {"Length": path.stat().st_size, "Sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def checked_copy(source, target, expected):
    actual = identity(source)
    if actual["Sha256"].lower() != expected["Sha256"].lower() or actual["Length"] != expected["Length"]:
        raise ValueError(f"Frozen component differs: {source}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("umd", "icd", "kmd", "frozen-manifest", "helpers", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--default-start", action="store_true",
                        help="package automatic enablement after logon, with manual recovery")
    parser.add_argument("--candidate-kmd", type=Path,
                        help="reviewed bit14 or GPU clock-control artifact with manifest.json and signed-package")
    parser.add_argument("--native-boot", action="store_true",
                        help="arm .158 for direct driver startup before logon; no PnP restart task")
    args = parser.parse_args()
    if args.candidate_kmd and not args.default_start:
        parser.error("--candidate-kmd requires --default-start")
    if args.native_boot and not args.default_start:
        parser.error("--native-boot requires --default-start")
    root = Path(__file__).resolve().parents[2]
    umd = json.loads((args.umd / "manifest.json").read_text())
    icd = json.loads((args.icd / "manifest.json").read_text())
    frozen = json.loads(args.frozen_manifest.read_text(encoding="utf-8-sig"))
    if frozen["kmd_version"] != "0.1.0.158" or frozen["kmd"]["tegragpurender.sys"]["Sha256"] != "46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088":
        raise ValueError("Only the retained visible-desktop .158 KMD is allowed")
    args.out.mkdir(parents=True, exist_ok=False)
    manifest = {"scope": "tgd3d-vulkan trial; retain the active .158 after testing; recovery is manual",
                "automatic_recovery": False,
                "recovery_target": "0.1.0.158-gated",
                "kmd_version": frozen["kmd_version"], "kmd_source": frozen["kmd_source"],
                "umd_name": "TegraGpuD3DVk.dll", "umd_source": umd["mesa"], "icd_source": icd["mesa"],
                "sync_mode": umd.get("sync_mode", "gpu-bridge"),
                "tools_source": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "kmd": frozen["kmd"], "app": {}, "helpers": {}, "files": {}}
    if args.default_start:
        manifest["scope"] = "frozen .158 with automatic tgd3d enablement after logon; manual testing"
        manifest["startup_mode"] = "logon-default"
    kernel = args.kmd
    if args.candidate_kmd:
        candidate = json.loads((args.candidate_kmd / "manifest.json").read_text())
        changed = {"tegra-x1-wddm/platform/t214_gr_context.c",
                   "tegra-x1-wddm/tests/test_gr_context.c"}
        vidpn_fix = candidate["variant"] == "shader-output-bit14-gpu460-vidpn"
        clock_control = candidate["variant"] in ("shader-output-bit14-gpu460", "shader-output-bit14-gpu460-vidpn")
        native = candidate["variant"] == "shader-output-bit14-native-boot" or clock_control
        if native:
            changed |= {"tegra-x1-wddm/windows/native_start_test.c",
                        "tegra-x1-wddm/tests/test_native_start_windows.c"}
        if clock_control:
            changed |= {"tegra-x1-wddm/platform/t214_clock.c", "tegra-x1-wddm/platform/t214_clock.h",
                        "tegra-x1-wddm/tests/test_clock.c", "tegra-x1-wddm/tests/test_hardware_windows.c",
                        "tegra-x1-wddm/tools/build.py", "tegra-x1-wddm/windows/hardware.c",
                        "tegra-x1-wddm/windows/render_miniport.c"}
            if vidpn_fix:
                changed |= {"tegra-x1-wddm/windows/panel_vidpn.c",
                            "tegra-x1-wddm/tests/test_panel_timings_windows.c"}
                if candidate.get("modeset_latch_patch_commit"):
                    changed |= {"tegra-x1-wddm/windows/panel_scanout.c",
                                "tegra-x1-wddm/tests/test_panel_scanout_windows.c"}
                    if candidate.get("source_visibility_patch_commit"):
                        changed |= {"tegra-x1-wddm/windows/panel.c",
                                    "tegra-x1-wddm/windows/panel.h"}
            baseline_hash = ("59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81" if vidpn_fix else
                             "c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53")
            if (candidate.get("target_khz") != 460800 or
                candidate["baseline_signed_files"]["tegragpurender.sys"]["Sha256"] !=
                    baseline_hash):
                raise ValueError("Candidate must restore its reviewed .158 clock baseline")
        if (candidate["base"] != frozen["kmd_source"] or
            candidate["variant"] not in ("shader-output-bit14", "shader-output-bit14-native-boot", "shader-output-bit14-gpu460", "shader-output-bit14-gpu460-vidpn") or
            not candidate.get("windows_signature_verified") or
            set(candidate["changed_files"]) != changed):
            raise ValueError("Unreviewed kernel variant or unsigned artifact")
        manifest["baseline_kmd"] = frozen["kmd"]
        if clock_control:
            manifest["baseline_kmd"] = candidate["baseline_signed_files"]
            manifest["kmd_gpu_khz"] = 460800
            manifest["kmd_clock_patch_commit"] = candidate["clock_patch_commit"]
            if vidpn_fix:
                manifest["kmd_vidpn_patch_commit"] = candidate["vidpn_patch_commit"]
                if candidate.get("modeset_latch_patch_commit"):
                    manifest["kmd_modeset_latch_patch_commit"] = candidate["modeset_latch_patch_commit"]
                    if candidate.get("source_visibility_patch_commit"):
                        manifest["kmd_source_visibility_patch_commit"] = candidate["source_visibility_patch_commit"]
        manifest["kmd_variant"] = candidate["variant"]
        manifest["kmd_patch_commit"] = candidate["patch_commit"]
        if native:
            manifest["kmd_startup_patch_commit"] = candidate["startup_patch_commit"]
        if candidate.get("inf_identity"):
            manifest["kmd_package_identity"] = candidate["inf_identity"]
        if candidate.get("additional_patch_commits"):
            manifest["kmd_additional_patch_commits"] = candidate["additional_patch_commits"]
        manifest["kmd"] = candidate["signed_files"]
        manifest["scope"] = ".158 bit14 fix with automatic tgd3d enablement after logon; manual testing"
        if clock_control:
            manifest["scope"] = (".158 GPU460.8 VidPN source-mode correction; restore the retained GPU460.8 kernel" if vidpn_fix else
                                 ".158 bit14 GPU460.8 control; restore the retained 729.6 MHz kernel; logon start")
            if manifest.get("kmd_modeset_latch_patch_commit"):
                manifest["scope"] = ".158 GPU460.8 VidPN with synchronous modeset latch; restore the retained GPU460.8 kernel"
            if manifest.get("kmd_source_visibility_patch_commit"):
                manifest["scope"] = ".158 GPU460.8 source visibility preserves primary binding; restore the retained GPU460.8 kernel"
        kernel = args.candidate_kmd / "signed-package"
        for name, record in manifest["baseline_kmd"].items():
            checked_copy(args.kmd / name, args.out / "baseline-kmd" / name, record)
    if args.native_boot:
        if manifest.get("kmd_variant") not in ("shader-output-bit14-native-boot", "shader-output-bit14-gpu460", "shader-output-bit14-gpu460-vidpn"):
            raise ValueError("Direct boot requires persistent native-start kernel support")
        manifest["startup_mode"] = "native-boot"
        manifest["scope"] = ".158 direct native startup before logon; owner-operated restart and application tests"
    for name, record in manifest["kmd"].items():
        checked_copy(kernel / name, args.out / "package" / name, record)
    for directory, component in ((args.umd, umd), (args.icd, icd)):
        for name, sha in component["files"].items():
            src = directory / name
            record = identity(src)
            if record["Sha256"] != sha:
                raise ValueError(f"Built artifact differs: {src}")
            checked_copy(src, args.out / "app" / name, record)
            manifest["app"][name] = record
    for name in ("display-state.exe", "desktop-path.exe", "trace-flush.exe", "etl-export-full.exe", "display158.aml"):
        record = frozen["helpers"][name]
        checked_copy(args.helpers / name, args.out / name, record)
        manifest["helpers"][name] = record
    scripts = root / "tegra-x1-wddm/tools/tgd3d-trial"
    default_shared = {"Common.ps1", "Prepare.ps1", "Enable-RuntimeAppContainerRead.ps1",
                      "Read-PlatformState.ps1", "Restore-AfterBoot.ps1",
                      "restore-preference.ps1", "Stop-DxgBootTrace.ps1", "tool-provenance.json",
                      "Switch-Kmd.ps1"}
    for file in scripts.iterdir():
        if args.default_start and file.name not in default_shared:
            continue
        if file.suffix in (".ps1", ".cmd", ".json", ".md"):
            shutil.copyfile(file, args.out / file.name)
    if args.default_start:
        for file in (root / "tegra-x1-wddm/tools/tgd3d-default").iterdir():
            if file.is_file():
                shutil.copyfile(file, args.out / file.name)
    licenses = args.out / "licenses"
    licenses.mkdir()
    for source, target in ((root / "docs/license.rst", "Mesa.txt"),
                           (root / "subprojects/dxbc-spirv/LICENSE", "dxbc-spirv.txt")):
        shutil.copyfile(source, licenses / target)
    for file in sorted(args.out.rglob("*")):
        if file.is_file():
            manifest["files"][file.relative_to(args.out).as_posix()] = identity(file)
    (args.out / "trial-components.json").write_text(json.dumps(manifest, indent=2) + "\n")
    archive = args.out.with_suffix(".zip")
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as z:
        for file in sorted(args.out.rglob("*")):
            if file.is_file():
                z.write(file, file.relative_to(args.out).as_posix())
    with zipfile.ZipFile(archive) as z:
        if z.testzip():
            raise ValueError("Archive CRC failed")
        for name, record in manifest["files"].items():
            data = z.read(name)
            if len(data) != record["Length"] or hashlib.sha256(data).hexdigest() != record["Sha256"]:
                raise ValueError("Archive manifest differs: " + name)
    result = {"archive": str(archive.resolve()), **identity(archive), "files": len(manifest["files"]),
              "umd_source": umd["mesa"], "icd_source": icd["mesa"], "kmd_source": frozen["kmd_source"]}
    archive.with_suffix(".delivery.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
