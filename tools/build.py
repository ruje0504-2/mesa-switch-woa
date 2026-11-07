#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the ARM64 GPU backend objects and run its host format tests.

This produces a static library, not an installable WDDM driver or Vulkan ICD.
Use LLVM, an external mesa-switch checkout and official WDK/SDK Include roots.
"""
import argparse
from pathlib import Path
import subprocess

MESA_REVISION = "0ff52079ca9e9e837ed27eb95a8d239f439a1864"
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--mesa", required=True, type=Path)
parser.add_argument("--wdk-include", required=True, type=Path)
parser.add_argument("--sdk-include", required=True, type=Path)
parser.add_argument("--out", required=True, type=Path)
parser.add_argument("--cc", default="clang")
parser.add_argument("--clang-cl", default="clang-cl")
parser.add_argument("--lib", default="llvm-lib")
args = parser.parse_args()
revision = subprocess.check_output(
    ["git", "-C", str(args.mesa), "rev-parse", "HEAD"], text=True).strip()
if revision != MESA_REVISION:
    parser.error(f"mesa-switch must be at {MESA_REVISION}; got {revision}")
args.out.mkdir(parents=True, exist_ok=True)
mesa_src = args.mesa / "src"
mesa_includes = [mesa_src, mesa_src / "nouveau/headers",
                 mesa_src / "nouveau/headers/nvidia/classes"]
encoder = mesa_src / "nouveau/horizon/nouveau_horizon_gm20b.c"
core = [root / "core/gm20b_mmu.c", root / "core/report.c", encoder]
host = args.out / "test-gm20b-core"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                *[f"-I{p}" for p in mesa_includes], *map(str, core), str(root / "tests/test_core.c"),
                "-o", str(host)], check=True)
subprocess.run([str(host.resolve())], check=True)

includes = [args.wdk_include / "km/crt", args.wdk_include / "km",
            args.wdk_include / "shared", args.sdk_include / "shared",
            args.sdk_include / "ucrt"]
flags = ["--target=aarch64-pc-windows-msvc", "/nologo", "/c", "/kernel",
         "/GS-", "/Gy", "/Zl", "/O2", "/W4", "/WX", "/std:c11",
         "/clang:-ffreestanding", "-Wno-microsoft-anon-tag", "-Wno-pragma-pack",
         "-Wno-ignored-pragma-intrinsic", "-Wno-nonportable-include-path",
         "/D_ARM64_", "/DARM64", "/D_WIN64", "/D_WIN32_WINNT=0x0A00",
         "/DNTDDI_VERSION=0x0A00000C", "/DNDEBUG",
         "/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_0",
         *[f"/I{p}" for p in mesa_includes], *[f"/imsvc{p}" for p in includes]]
objects = []
for source in [*core, root / "windows/resources.c"]:
    obj = args.out / (source.stem + ".obj")
    subprocess.run([args.clang_cl, *flags, f"/Fo{obj}", "--", str(source)], check=True)
    objects.append(obj)
library = args.out / "TegraGpuCore.lib"
subprocess.run([args.lib, "/nologo", "/machine:arm64", f"/out:{library}",
                *map(str, objects)], check=True)
print(f"ARM64 kernel library: {library} (hardware execution not tested)")
