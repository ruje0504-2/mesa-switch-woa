# Claude 原生 Vulkan 桌面后端交接

本分支 `claude-vulkan` 保存 Claude 在 2026-11-12 留下的 **未完成实现**，
供 Claude 后续继续开发。当前任务已结束。本次只收回源码并整理交接，
没有继续实现、部署驱动或进行实机测试。

拟实现的路径是 **D3D10 WDDM DDI → Vulkan → NVK → GM20B**。
它是现有 Zink 桌面后端的独立候选；应用侧 DX9/10/11 的 DXVK 适配是另一项任务。

## 来源与分支基线

- Claude 实现起点：`8fcf9e07f542968c22f99f7aa4c660ab6495b88c`。
- 本分支建立在主线 `7a771aa6602d2c94cb4cd362855777290a2503f2` 上，
  保留主线后续的纹理、着色器缓存和 Win32 timeline 同步改动。
- 从 VPS 收回 25 个源码/构建文件；与先前收回的源码快照一致。
  24 个文件逐字节相同；根目录 `meson.build` 只应用 Claude 的新增配置，
  同时保留主线已有的 Windows shader-cache 改动。
- 新增代码包括 adapter/device、资源与视图、DXBC 着色器、绘制与状态、
  命令队列同步、DXGI 回调、上传缓冲和 ETW 测点。存在源码不代表这些路径已验证。
- `tgd3d` 默认关闭；实验 DLL 名称默认为 `TegraGpuD3DVk`。

## 已有验证与已知未完成部分

最后一次远端构建是 Windows x86-64 交叉编译，**失败**。
现存日志至少包含以下错误，本次原样保留源码，未修复：

- `winddk_compat.h` 使用 `LARGE_INTEGER` 时类型尚未定义。
- `tgd3d_cmd.cpp` 中 `OBJECT_ATTRIBUTES` 类型不完整。
- `ClearRenderTargetView` 的 `const FLOAT *` 参数与 DDI 函数指针要求的
  `FLOAT *` 不匹配。

`tests/meson.build` 仍是占位文件，没有完成后端行为测试。
没有本后端的 ARM64 成功构建、GM20B 像素回读、DWM 上屏或性能验收结果。
旧 Zink、NVK 的控制测试与 `.87` / `.158` 历史实机结果不能作为本后端通过的证明。

后续需由 Claude 修正构建、补齐测试与实际缺失的路径，核对资源共享/格式布局、
runtime 与 NVK 双向逐提交同步、对象生命周期、能力声明和呈现节拍。
新声明的 NVK 私有 instance 结构也需要完成生产端/消费端接入核对。

远端构建配置使用 `-Dtgd3d=true -Dbuild-tests=true`、Windows 平台，
关闭 Gallium、Vulkan 驱动构建、OpenGL、EGL、GLX、LLVM 和 shader-cache，
依赖 LLVM-MinGW 与 SDK/WDK 10.0.26100.0。
完整源码快照、原始 `env.sh` / `setup.sh`、构建日志及 probe 已收回本地交接目录，
不是可安装驱动包。后续需整理可移植的 ARM64 构建入口。

## 上游与许可证

- 新增 tgd3d 源文件保留 Claude 交付的 `SPDX-License-Identifier: MIT`。
- DXBC → SPIR-V 依赖 [dxbc-spirv](https://github.com/doitsujin/dxbc-spirv)，
  wrap 固定到 `bf14419e5fa7eacb817b7b632f03cb61d61bbad7`。
  保留上游 Philip Rebohle 的 [MIT 许可证](third_party/dxbc-spirv.LICENSE)。
  依赖通过 `subprojects/dxbc-spirv.wrap` 获取，未把第三方构建树复制进项目。
- 本后端不使用 Dozen；Dozen 的翻译方向与本任务不同。

继续开发时保持动态 512 MiB GPU 预算、现有睡眠和风扇策略。
Direct Flip 暂缓；不调查黑屏/重启原因，不进行具体游戏专项排错。
GPU 源码及后续提交仍统一保存在本仓库。
