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
parser.add_argument("--link", default="lld-link")
parser.add_argument("--dlltool", default="llvm-dlltool")
parser.add_argument("--probe", action="store_true", help="also build the opt-in KMDF probe and its SwitchFan bridge")
parser.add_argument("--keep-fan", action="store_true", help="with --probe, preserve the existing signed SwitchFan package")
parser.add_argument("--windows-tests", action="store_true", help="build the real-WDK paging test executable for ARM64 Windows")
args = parser.parse_args()
if args.keep_fan and not args.probe:
    parser.error("--keep-fan requires --probe")
revision = subprocess.check_output(
    ["git", "-C", str(args.mesa), "rev-parse", "HEAD"], text=True).strip()
if revision != MESA_REVISION:
    parser.error(f"mesa-switch must be at {MESA_REVISION}; got {revision}")
args.out.mkdir(parents=True, exist_ok=True)
mesa_src = args.mesa / "src"
mesa_includes = [mesa_src, mesa_src / "nouveau/headers",
                 mesa_src / "nouveau/headers/nvidia/classes"]
encoder = mesa_src / "nouveau/horizon/nouveau_horizon_gm20b.c"
core = [root / "core/gm20b_mmu.c", root / "core/report.c", root / "core/gpfifo.c",
        root / "core/execute.c", root / "platform/t214_frontend.c", encoder]
host = args.out / "test-gm20b-core"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                *[f"-I{p}" for p in mesa_includes], *map(str, core), str(root / "tests/test_core.c"),
                str(root / "tests/test_execute_init.c"),
                "-o", str(host)], check=True)
subprocess.run([str(host.resolve())], check=True)
power = root / "platform/t214_power.c"
power_test = args.out / "test-t214-power"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(power), str(root / "tests/test_power.c"), "-o", str(power_test)], check=True)
subprocess.run([str(power_test.resolve())], check=True)
clock = root / "platform/t214_clock.c"
clock_test = args.out / "test-t214-clock"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(clock), str(root / "tests/test_clock.c"), "-o", str(clock_test)], check=True)
subprocess.run([str(clock_test.resolve())], check=True)
frontend_test = args.out / "test-t214-frontend"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(root / "platform/t214_frontend.c"), str(root / "tests/test_frontend.c"),
                "-o", str(frontend_test)], check=True)
subprocess.run([str(frontend_test.resolve())], check=True)
tlb = root / "platform/t214_mmu.c"
tlb_test = args.out / "test-tlb"
subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                str(tlb), str(root / "tests/test_tlb.c"), "-o", str(tlb_test)], check=True)
subprocess.run([str(tlb_test.resolve())], check=True)

includes = [args.wdk_include / "km/crt", args.wdk_include / "km",
            args.wdk_include / "shared", args.sdk_include / "shared",
            args.sdk_include / "ucrt"]
flags = ["--target=aarch64-pc-windows-msvc", "/nologo", "/c", "/kernel",
         "/GS-", "/Gy", "/Zl", "/O2", "/W4", "/WX", "/std:c11",
         "/clang:-ffreestanding", "-Wno-microsoft-anon-tag", "-Wno-pragma-pack",
         "-Wno-ignored-pragma-intrinsic", "-Wno-nonportable-include-path",
         "/D_ARM64_", "/DARM64", "/D_WIN64", "/D_WIN32_WINNT=0x0A00",
         "/DNTDDI_VERSION=0x0A00000C", "/DNDEBUG",
         "/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_4",
         *[f"/I{p}" for p in mesa_includes], *[f"/imsvc{p}" for p in includes]]
objects = []
for source in [*core, power, clock, tlb, root / "windows/resources.c",
               root / "windows/private_memory.c", root / "windows/paging.c",
               root / "windows/table_segment.c", root / "windows/root_table.c",
               root / "windows/paging_buffer.c"]:
    obj = args.out / (source.stem + ".obj")
    subprocess.run([args.clang_cl, *flags, f"/Fo{obj}", "--", str(source)], check=True)
    objects.append(obj)
library = args.out / "TegraGpuCore.lib"
subprocess.run([args.lib, "/nologo", "/machine:arm64", f"/out:{library}",
                *map(str, objects)], check=True)
print(f"ARM64 kernel library: {library} (hardware execution not tested)")

if args.windows_tests:
    test_obj = args.out / "test_paging_windows.obj"
    subprocess.run([args.clang_cl, *flags, "/clang:-funwind-tables", f"/Fo{test_obj}", "--",
                    str(root / "tests/test_paging_windows.c")], check=True)
    # The SDK header NuGet does not ship import libraries. Use the same LLVM
    # import-library construction as the existing performance tool.
    exports = args.out / "paging-kernel32.def"
    exports.write_text("LIBRARY kernel32.dll\nEXPORTS\nExitProcess\nGetStdHandle\nWriteFile\n")
    sdk_lib = args.out / "paging-kernel32.lib"
    subprocess.run([args.dlltool, "-m", "arm64", "-d", str(exports), "-l", str(sdk_lib)], check=True)
    subprocess.run([args.link, "/nologo", "/machine:arm64", "/subsystem:console,10.0",
                    "/entry:start", "/include:_load_config_used", "/nodefaultlib", "/release", "/opt:ref", "/timestamp:0",
                    f"/out:{args.out / 'test-paging.exe'}", str(test_obj),
                    str(args.out / "paging.obj"), str(args.out / "gm20b_mmu.obj"),
                    str(args.out / "table_segment.obj"), str(args.out / "private_memory.obj"),
                    str(args.out / "root_table.obj"), str(args.out / "t214_mmu.obj"),
                    str(args.out / "paging_buffer.obj"),
                    str(sdk_lib)], check=True)

if args.probe:
    import shutil
    kit = args.out / "kit"
    wdk_root = args.wdk_include.parent.parent
    wdf_inc = args.wdk_include.parent / "wdf/kmdf/1.15"
    wdf_lib = wdk_root / "Lib/wdf/kmdf/ARM64/1.15"
    km_lib = wdk_root / "Lib" / args.wdk_include.name / "km/ARM64"
    repo = root.parents[1]
    kflags = [*flags, "/DKMDF_VERSION_MAJOR=1", "/DKMDF_VERSION_MINOR=15",
              "/DPOOL_NX_OPTIN=1", "/DSWITCH_LITE_GPU=1", "-Wno-unused-parameter",
              "-Wno-invalid-token-paste", "-Wno-unknown-pragmas", "-Wno-ignored-attributes",
              f"/imsvc{wdf_inc}", f"/imsvc{args.wdk_include / 'km/spb/1.0'}"]
    support = args.out / "kmdf_support.obj"
    subprocess.run([args.clang_cl, *kflags, f"/Fo{support}", "--", str(repo / "tools/kmdf_support.c")], check=True)
    packages = [("tegragpuprobe", root / "windows/probe.c", root / "windows/tegragpuprobe.inf")]
    if not args.keep_fan:
        packages.append(("switchfan", repo / "drivers/SwitchFan/src/switchfan.c", repo / "drivers/SwitchFan/switchfan.inf"))
    for name, source, inf in packages:
        dest = kit / name
        dest.mkdir(parents=True, exist_ok=True)
        obj = args.out / (name + ".obj")
        subprocess.run([args.clang_cl, *kflags, f"/Fo{obj}", "--", str(source)], check=True)
        libs = [wdf_lib / "wdfdriverentry.lib", wdf_lib / "wdfldr.lib",
                km_lib / "ntoskrnl.lib", km_lib / "hal.lib", km_lib / "ntstrsafe.lib",
                km_lib / "bufferoverflowfastfailk.lib"]
        if name == "tegragpuprobe": libs.append(library)
        subprocess.run([args.link, "/nologo", "/driver", "/subsystem:native,10.0", "/machine:arm64",
                        "/entry:FxDriverEntry", "/nodefaultlib", "/release", "/opt:ref", "/opt:icf",
                        "/tsaware:no", "/section:.text,ERP", "/section:.rdata,RP",
                        "/section:.data,RWP", "/section:.pdata,RP", "/section:INIT,ERD",
                        "/timestamp:0", f"/out:{dest / (name + '.sys')}",
                        str(obj), str(support), *map(str, libs)], check=True)
        shutil.copyfile(inf, dest / inf.name)
    built = "probe (existing SwitchFan package preserved)" if args.keep_fan else "probe/bridge"
    print(f"Unsigned experimental {built}: {kit}; requires catalog generation and signing.")
