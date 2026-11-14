# .158 + tgd3d 启动包

当前实机采用 `logon-default`：进入系统后由5秒延迟任务启用tgd3d。
直接原生冷启动尚未通过；下文的 `native-boot` 为显式诊断选项。

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
