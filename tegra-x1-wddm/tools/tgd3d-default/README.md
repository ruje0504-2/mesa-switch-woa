# .158 + tgd3d 启动包

当前实机采用 `logon-default`：进入系统后由5秒延迟任务启用tgd3d。
直接原生冷启动尚未通过；下文的 `native-boot` 为显式诊断选项。

## 当前缓存更新

2026-11-14已在活动`.158`组合上接入shader-object磁盘缓存，双向GPU Fence开启。
完整当前组件和验证范围见[交接记录](../../docs/TGD3D_COLD_BOOT_HANDOFF_20261113.md)。

`Update-ShaderCache.ps1 -CandidateDirectory <目录> -Restart`仅用于该次缓存专用UMD
更新。候选目录需包含`update.json`、ARM64 UMD、配套控制程序、同候选的原生CPU控制
结果`controls-state.json`，以及两个更新/恢复脚本。清单固定更新前的目录和组件哈希，
不接受任意版本。更新器复制已有受保护包到新目录，替换UMD及配套控制程序，保留KMD、
ICD、恢复文件和平台设置；切换注册及登录任务，下一次重启生效，不覆盖已映射DLL。

缓存使用当前令牌的LocalAppData下`mesa_shader_cache_win32-*`目录，每个缓存目录
上限128MiB；这是磁盘容量，不占用512MiB GPU预算。缓存命中仍在当前VkDevice上
重建shader object，不跨设备复用句柄。DXBC→SPIR-V转换仍会执行；缓存主要节省NVK/NAK
编译。源码已有缓存不代表旧冻结UMD已启用，构建必须同时包含缓存源码及
`ENABLE_SHADER_CACHE`后端。

本轮需要撤回时，在**新包目录**以管理员身份运行`Restore-ShaderCache.ps1 -Restart`。
它恢复保存的旧UMD/ICD注册、包目录和登录任务，重启后加载旧UMD；保留原`.158` KMD、
ICD二进制和频率，不安装`.59`。恢复入口与更新后的实际路径写入交接记录。
脚本及哈希核对通过；未为验证恢复工具而额外回退、重启或重复安装。

## 历史冷启动诊断包

本包使用清单中指定的 `.158` KMD 和配套 ARM64 NVK ICD。
`shader-output-bit14-native-boot` 变体在冻结 `.158` 上只移植 bit14 修复及显式
持久原生启动门控；附带原签名 KMD 的恢复文件。UMD为CPU同步对照版，双向GPU
Fence桥接仍屏蔽。该组合用于用户测试，稳定性尚未验收通过。

1. 当前应为已安装的 `.158 / Code43` 门控桌面，以管理员身份运行 `INSTALL.cmd`。
2. 安装完成后自行重启 Windows。清单 `startup_mode: native-boot` 使用 KMD 的
   `AllowNativeBoot` 持久授权，开机设备初始化即进入原生驱动，可重复 StartDevice
   和重启；不依赖一次性许可、登录或启动任务，不在活动桌面上重启显卡。
3. 不会自动开测试窗口、采集Dump、回退`.59`或重启Windows。CPU、风扇和睡眠设置保持。

旧 `logon-default` 包仍在登录后约5秒启用，不属于直接原生开机模式。
此前使用一次性许可加启动任务续写的方案已被替换；不能用于持久原生启动验收。

需要恢复时，以管理员身份运行 `RESTORE.cmd`。它先关闭自动任务、清除
`AllowNativeBoot` 与测试许可；如原生 GPU 正在运行，会提示手动重启。
重启后再运行一次，恢复原签名 `.158` 并保持启动门控关闭，不热切换活动GPU，
不安装`.59`。完整组件来源和SHA256见 `trial-components.json`。

`shader-output-bit14-gpu460` 是GPU460.8MHz的电源对照，初始使用登录后启用。
它保留800mV固定下限，CPU及UMD/ICD不由KMD修改；内存1600MHz通过WinX/Hekate
启动配置设置。安装前必须关闭当前自动启用任务并重启到`.158 / Code43`，不在
原生桌面上热切换KMD。启动参数中的`BootGpuMeasuredKhz`来自启动时硬件计数器，
`BootGpuRailUv`为启动时PMIC设定回读，不代表负载瞬态电压。
按用户指定顺序，桌面测试通过后才将同一组合切换为`native-boot`，进行冷启动测试。
`Native-Boot.ps1`可在匹配的活动`.158`上配置下一次启动并停用登录任务，不重启显卡。

该对照包的`RESTORE.cmd`恢复带bit14的729.6MHz基线SYS（SHA256以`c629328e`开头），
不是更早的原始`.158`。恢复后先保持门控桌面；需要恢复原来的登录后tgd3d启动时，
从原基线包运行`Install-Default.ps1 -Prepared`，再按其提示启用。
