# D3D10umd Readme

## Tegra NVK backend

`-Dgallium-d3d10umd=true -Dgallium-d3d10-backend=nvk -Dgallium-drivers=zink`
builds an experimental hardware target. It queries the runtime's adapter
callback for the Tegra ABI and opens Zink on that exact Vulkan device LUID.
The KMD must enable native submissions. This target exports the D3D adapter
entry points without the software target's KMT emulation.

The frontend currently negotiates D3D10 DDIs. D3D10.1/D3D11 DDIs, runtime
resource sharing and native presentation still need implementation. The
hardware target returns an unsupported result from presentation callbacks;
it does not use the GDI target's frontbuffer path. It is not a working DWM
driver yet. [Windows build and verification](../../../../tegra-x1-wddm/docs/PORTING.md#direct3d-umd-and-dwm)

`nvk_d3d_check.exe` explicitly exercises native DDI clear/copy/readback and
SM4 vertex/pixel shaders with ordinary and indexed/scissored triangle draws
on a selected KMT LUID. Every pixel is checked against the expected image.
Run it only with a native-submission KMD and registered NVK ICD; elevated
Windows processes ignore its `VK_DRIVER_FILES` override.
This probe does not use the Windows D3D runtime or presentation callbacks.

`nvk_d3d_runtime_check.exe --luid 16-hex-digits` instead calls the Windows
`D3D11CreateDevice` runtime at feature level 10_0 and checks RGBA/BGRA clear,
SM4 drawing and staging readback. It requires the registered native UMD,
checks the resulting adapter LUID and loaded `TegraGpuD3D.dll`, and does not
fall back to WARP. `--warp` and `--software path/to/gdi-reference.dll` are
explicit software controls. These rendering modes do not test resource
sharing or presentation; none runs as a default Meson test.
For the experimental NVK hardware path, register the ICD as above, make the
ARM64 Vulkan loader available and set `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`.
Append `--implicit-release` to the hardware command to retain scope-exit
ComPtr destruction while investigating the exit-path fault. Exception
diagnostics print raw addresses and a bounded ARM64 frame-pointer chain;
module bases are recorded before drawing, avoiding loader queries during
failure reporting. This is not a full unwind or a teardown fix.

`--luid 16-hex-digits --shared-contract` checks runtime resource association,
opening and release for legacy and NT shared BGRA textures, including the
consumer's descriptor after producer release. `--warp --shared-contract`
provides a software control. This mode submits no drawing/copy commands and
does not validate shared image contents or synchronization. The production
UMD's runtime resource bridge is still incomplete.
[Runtime contract evidence](../../../../tegra-x1-wddm/docs/D3D_RUNTIME_SHARING.md)

Append `--feature-levels` to `--luid 16-hex-digits`, `--warp` or `--software
path/to/dll` to probe 11_1, 11_0, 10_1 and 10_0 separately, then negotiate
the full ordered list. It records HRESULT, selected level, adapter identity
and native UMD path. It does not render or claim DWM acceptance. The normal
rendering regression deliberately requests only 10_0, so that test alone
does not establish a feature-level ceiling. The present frontend's ceiling
also follows from `SUPPORT_D3D10_1=0`, `SUPPORT_D3D11=0` and its supported
device-interface table; advertising 11_0 requires implementing its DDIs.

## GDI software backend

When compiled with `gallium-driver=llvmpipe` or `gallium-driver=softpipe` the resulting libgallium_d3d10.dll implements D3D10's software rendering interface, like WARP.

It can be used directly from WLK 1.6 and WHCK 2.0 D3D10+ tests, via the -Src
and -SWDLL options. For example:

    wgf11blend.exe -Debug -DoNotCatchExceptions -DXGI:1.1 -FeatureLevel:10.0 -Src:SW -SWDLL:libgallium_d3d10.dll -LogClean -LogVerbose

However, as of WHCK version 2.1 this mechanism no longer works reliably.
Either you use WHCK 2.0 binaries, or you must use the alternative method
described below (of copying libgallium_d3d10.dll into the executable directory and rename
it such that it matches the D3D10 UMD of the test machine).

Examples can be easily modified to load it too:

    D3D10CreateDeviceAndSwapChain(NULL,
                                  D3D10_DRIVER_TYPE_SOFTWARE,
                                  LoadLibraryA("libgallium_d3d10"), /* Software */
                                  Flags,
                                  D3D10_SDK_VERSION,
                                  &SwapChainDesc,
                                  &g_pSwapChain,
                                  &g_pDevice);

    D3D11CreateDeviceAndSwapChain(NULL, /* pAdapter */
                                  D3D_DRIVER_TYPE_SOFTWARE,
                                  LoadLibraryA("libgallium_d3d10"), /* Software */
                                  Flags,
                                  FeatureLevels,
                                  sizeof FeatureLevels / sizeof FeatureLevels[0],
                                  D3D11_SDK_VERSION,
                                  &SwapChainDesc,
                                  &g_pSwapChain,
                                  &g_pDevice,
                                  NULL, /* pFeatureLevel */
                                  &g_pDeviceContext); /* ppImmediateContext */

Alternatively, it can be renamed into the system's D3D10 UMD driver (e.g.,
vm3dum_10.dll for VMware vGPU, nvwgf2um.dll for NVIDIA GPU), and placed into
the application directory, or system's directory, and used instead.

For the DLL to be picked from the application directory you'll need to do the
following once:

    reg add "HKLM\System\CurrentControlSet\Control\Session Manager" /v "SafeDllSearchMode" /t REG_DWORD /d 0 /f

See also <https://docs.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order>

## Building with MSVC

Install Windows SDK 10.0.26100.0 and Windows DDK 10.0.26100.0 with MSVC and use it, other version of Windows SDK/DDK
may also work but not verified.

## Building with mingw

Install Windows SDK 10.0.26100.0 and Windows DDK 10.0.26100.0, other version of Windows SDK/DDK
may also work but not verified.

Copy the following list of files from Windows SDK/DDK into mingw's `include` folder or `include\winddk` folder in mesa source tree:
    ```txt
    d3d10umddi.h
    d3d11TokenizedProgramFormat.hpp
    d3dkmddi.h
    d3dkmdt.h
    d3dkmthk.h
    d3dukmdt.h
    d3dumddi.h
    dxgiddi.h
    dxmini.h
    wmidata.h
    ```
