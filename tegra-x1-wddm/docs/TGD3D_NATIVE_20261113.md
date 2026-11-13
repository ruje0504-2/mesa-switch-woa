# tgd3d 原生桌面试验与异常重启

状态：短时原生上屏，稳定性失败。当前开发分支为 `tgd3d-vulkan`，原名
`claude-vulkan`。本轮没有五分钟、已知像素、硬件旋转或 vkcube 通过结果。

| 组件 | 源码提交 | SHA256 |
| --- | --- | --- |
| 签名 .158 KMD | `6c854e9f394c3d761fab85885bead4bbd4588581` | `46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088` |
| TegraGpuD3DVk.dll | `3c5d327daa77b40ca1e822d5270e285a71a61807` | `8232217dbedeee4c4b61c08a319b2021b2889f6a618ab1a975c89f86823c0a64` |
| ARM64 NVK ICD | `7c677212e5843725f3dbb12ee0b9dad6b1339a05` | `17dc3110393365eef279a90104850d6f517c74659441916266afeae0ae2c8f73` |

Windows 启动事件记录版本 `10.0.22621.2861`。交付工具提交为 `6c1340c3`。
原测试包 SHA256 为 `88b8220db9c5eb7b129e057698bbfe07989d1d89255d28a1428669b8f8740198`。

控制脚本在 2026-11-13 05:06:20 UTC 记录 .158 Code 0、Hardware 0、Panel 0、
一次性许可消费为 0。原生活动显示路径与 KMT LUID 都是
`000000000009b66c`，720×1280。DWM PID 800 的独立 tgd3d provider 保留
NVK 同 LUID、CreateDevice/S_OK 和 251 次 Present/S_OK；用户随后确认画面正常、
能够操作。此后用户打开桌面的 CPU/风扇/温度程序并观察到自动重启。

桌面入口事后只读核对为无参数的 `LitePerformanceCheck.exe`，34304 字节，
SHA256 `ca127482bbb59ac6c1b2f576f6b9793231c1b865a00057ec1171892f210929d4`。
这是时间顺序证据，不能据此认定该程序、UMD、KMD 或某个系统驱动是根因。

保留主 ETL 导出 1621 个 tgd3d 事件及 277190 个原始记录。未保留 DWM error
事件或 KMD recovery 事件；其它进程的 `0x887b0001` 位于 staging Map 的
DONOTWAIT 路径。最后保留的 DMA 和原生 Flip 有完成记录。主 ETL 的 EndTime
和 BuffersWritten 为 0，尾部不完整，不能用“没有错误事件”证明没有错误。
系统记录 6008/41，BugcheckCode 为 0，没有可用 minidump。系统事件、ETW 和
控制脚本时钟存在偏移，保留各自原值/RecordId，不能混用它们计算精确崩溃时长。

## 验收工具问题

1. 固定 `desktop-path.exe`（`6a10be74…`）先输出 DisplayConfig/KMT 两条记录，
   再查 argv DLL 的 `TgGpuReadD3DAdapterProbe` 私有导出。冻结的 no3d 桥没有该
   导出，工具在执行 D3D11CreateDevice 之前退出 4。这正是所选只读路径；原断言
   错误地要求退出 0。修正要求该已知桥/工具的退出 4、两条完整记录、KMT S_OK、
   活动路径/LUID 匹配，并另用真正 DWM 的 ETW 判断 UMD 创建和呈现。
2. ETW 转换后的 FILETIME 相对控制脚本墙钟约偏移 15 秒，会误滤除已发生的
   CreateDevice。下一版 live consumer 使用原始 QPC，启动请求也记录 QPC；
   UMD v2 事件携带发出进程的创建 FILETIME，以精确匹配当前 DWM，而非用两个
   时钟的先后关系推断身份。v1 数据仍可离线解码。
3. 初次目录枚举把 `umd-live.jsonl` 长度报为 210，但其 SHA256 与归档中的
   291273 字节完全一致。随后用同一个文件句柄计量长度和计算 SHA256，全部
   144 个文件、154786727 字节匹配。原清单和文件句柄清单均保留。

时钟处理依据微软 [EVENT_TRACE_LOGFILEW](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_logfilew)
的 ProcessTraceMode，以及 [AutoLogger 配置](https://learn.microsoft.com/en-us/windows/win32/etw/configuring-and-starting-an-autologger-session)
的 QPC ClockType=1。runtime callback 审查依据
[Changes from Direct3D 10](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)
（页面更新日期 2021-12-15）：线程规则依赖 DDI 与 FREETHREADED 声明，不能把
D3D11 放宽规则直接用于未声明该能力的 D3D10 路径。

## 恢复与下一步

异常重启后门控关闭，重复启动防护要求回退，没有再次原生启动。
05:12:25 UTC 显示 restore/verify 均为 0；05:20:46 UTC 再核对 .59/Code 0、
原 ACPI、CPU 1497600 kHz、风扇13和原电源设置，临时任务/许可/注册/ETW均清理。
证据 zip 为15894489字节，SHA256
`1b5e5b9b623a63d0d7a6709b74c8a9e148348bd7b7d2aa07bce4f796d54dc4f8`。
证据在独立产物目录 `hardware/`，逐文件核验见 `verified.json`、
`evidence-manifest-stream.json`，恢复见 `handoff-verified.json`。

先修复上述验收问题，再针对异常重启形成有证据支持的单项补丁。复测按约30秒静止、
约60秒简单开窗、五分钟交互逐步推进；CPU/风扇/温度程序为独立回归步骤。
工具修正或控制测试通过不构成重启已修复，也不能替代物理观察。

## 验收修正的控制验证

提交 `4309434c8b2f087883e673515df16aa346f5903d` 只修改取证关联与相关工具，
没有修改 KMD、ICD 或 Vulkan 绘制/同步逻辑。ARM64 UMD SHA256 为
`d90404c27cfa999aad9b6645289458599c7a41abdfe782d883543cf815f0273e`。
构建时已跟踪源码干净，唯一未跟踪项是 `.DS_Store`，清单如实记录。

2026-11-13 05:49:57 UTC，在 `.59 / Code 0` 上通过20个脚本的语法检查、
18项读取判定（包括墙钟前后偏移、PID复用、错误时钟/频率）、11项CPU合约检查，
并用真实Windows ETW传输6条合成事件，实时与离线字段一致、3个完整实时缓冲状态。
新版解码器还读回旧v1的全部1621条事件，不为旧事件捏造进程创建时间。
19个控制证据文件/665881字节已按长度和SHA256取回校验。
控制证据zip SHA256为
`44626b595c51155b2e65761d600f302c3950c8523bee7c0bcb2a3c4ebe59c86d`。
这些是工具和CPU控制，不是GPU验证或重启问题已修复的证据。

随后在 `.59` 进行同一性能程序的无参数对照：进程7776持续运行30.18秒，
06:01:53 UTC正常退出0，控制进程连续运行，GPU仍为`.59`，没有重启。
这仅证明该次基线运行成功，不能排除程序I/O与原生路径的交互问题。

## 第二轮：静止阶段通过，开窗阶段异常重启

本轮只更新取证工具和分阶段观察，没有修改绘制、同步或 KMD。组件为上述
`4309434c8b2f087883e673515df16aa346f5903d` UMD、原 `7c677212` ICD 和冻结
`.158` KMD；完整组件 SHA256 见前述清单。阶段工具为 `b4942e7a`，测试包
6718393 字节，SHA256
`2a7ce341d84915c0287fed038e578d311f235897c46893f4451b73ddc8e80833`。

试验目录为 `tgd3d-4309434c-20261113-142254`。Windows 仍为
`10.0.22621.2861`；WMI 启动时间为 `06:24:41.602617 UTC`，控制记录在
`06:26:56.6543149 UTC` 确认 `.158 / Code 0`、Hardware/Panel 0、许可消费为0。
原生 LUID 为 `0000000000086d7d`，DWM PID812，创建 FILETIME
`134390247145347024`。原始 QPC 与进程创建时间匹配了本轮 DWM 的 NVK 初始化、
设备创建和呈现，修正后的 helper 退出4判定也通过。

静止观察在 QPC `2935736109` 开始，频率19200000；0/15/30秒检查均成功，
30秒后截图，观察计时40.27秒。用户确认“画面正常，能操作”，该阶段通过。
随后请求60秒开窗阶段，用户报告“已经死机重启了”，并确认“尚未看到彩色窗口
就死机”。没有 Window observer 的动作/结果，也没有五分钟或性能程序回归结果。
用户另补充系统自带窗口可以使用，而新程序开窗时会重启；这是排查差异，尚非根因。

保留数据如下：

- 实时 tgd3d 日志2219条事件、92条状态，DWM 389次成功 Present；离线主 ETL
  多保留最后6条事件，合计2225条、DWM 391次成功 Present 和394次成功提交。
  DWM 的 Present 回压耗时均为0，保留的一次CPU等待属于读回；Explorer 有两次
  两帧上限产生的回压。跨队列 GPU Fence 已实现，不代表全部API路径都无CPU等待。
- 原始 ETW 339152条；已用该机模板解码335068条。1857条 Dwm-Core 203事件
  的载荷与模板长度不同，明确排除。没有保留 KMD recovery/execution 事件。
  最后的原生 DMA1885有完成记录，随后有 PresentId391 的 MPO 回调；尾部仍有
  排队记录。主 ETL EndTime/BuffersWritten 为0，不能用尾部未配对事件认定GPU挂起。
- 系统事件记录6008/41、BugcheckCode 0；没有对应 Minidump、MEMORY.DMP、
  LiveKernelReports、用户转储或 WER 报告。EventRecordId40235是异常重启，
  40289是恢复脚本正常请求重启，两者必须区分。墙钟经过校正，不能混算ETW、
  WMI与系统事件时间来推导崩溃耗时。
- 后续只读补查的 PowerShell 400事件36883确认控制进程PID3864启动过
  `Start-NativeStage.ps1 -Stage Window`。脚本块日志只能证明解析/加载，不能
  当作每条命令已执行。TaskScheduler Operational当时未开启。
  `stage-Window.request.json` 保留408字节全零内容，说明原请求记录未完整保存，
  不能据此证明 observer 已启动。后续工具改用 WriteThrough、Flush(true)、
  pending文件完成后发布，并携带控制进程PID/创建时间；不把这项取证修正当作重启修复。
  更新脚本已于06:47:18 UTC在Windows PowerShell通过语法解析（0错误），未执行
  新的原生动作，也没有用解析成功证明断电持久性。

06:33:42 UTC完成恢复核对：`.59 / Code 0`，SYS/ACPI哈希与原基线完全一致，
显示restore/verify均为0，CPU1497600kHz、风扇13、电源/睡眠设置保持，
临时任务、许可、UMD/ICD注册及ETW已清理。用户随后确认“已回到桌面，能操作”。
没有再次启动该失败组合。

证据163个文件、157525799字节，逐文件长度和SHA256全部匹配；归档18635015字节，
SHA256 `3b94140003b54590383533e28ddefbafd283e1b74cfa3a39c895d7d4ef7f306b`。
独立产物目录为 `hardware/staged-4309434c/`；归档、`verified.json`、
后补系统事件与物理反馈分别保存，未改写已归档的原始记录。

逐项审查 `main` 的 `0ced476d` 和 `7a1f7176`：前者修改 Gallium/Zink 的图像
映射与缓存范围，不在 tgd3d 直接调用链；后者扩充已有 TDR 回调的缓存报告，
不负责建立 SD 转储通道，也不能保证硬复位能留下转储。本轮均未移植或部署。
CreateProcess/CreateContext的冻结KMD实现与当前支线相同，审查参考微软
[CreateProcess](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_createprocess)
（2025-02-07）与
[CreateContext](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_createcontext)
（2024-02-22）；仅有新进程时间关联，不足以认定某个回调违约。

### 已安装的 SD 转储路径

按用户提醒核对主会话的存储安装记录及实机：06:45:46 UTC，SDMMC1
`ACPI\NVDA0101\0` 为 **0.5.1.2 / oem90.inf / Code 0**，48056字节 SYS，
SHA256 `5389b30d3a04fad330b9afef36b0d500ad363c6c5b39c55c9b60096669bfd945`。
与 Windows-for-Switch-Drivers 的
[固定安装记录](https://github.com/ruje0504-2/Windows-for-Switch-Drivers/blob/6cd7305e7e932b4df4bb4e53282fedd9d53e8bd8/drivers/Tegra210Sdhc/README.md#0512-installation-and-runtime-result)
匹配，记录的SD源码为 `5345692e4b7ec0d52e80937ae451bb2c52750fe8`。
该版本保留SDPORT的独立PIO转储支持；无需因GPU回退重新安装SD驱动。

实机 CrashDumpEnabled=3、AutoReboot=1，分页文件512MiB。补查到06:39 UTC的
System记录中，本轮开始之后没有volmgr事件；旧的45/46记录早于这轮。
这证明没有再次观察到初始化错误，仍不是成功捕获转储的证明。两次tgd3d异常重启
都没有可用转储。微软
[Event 41说明](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/event-id-41-restart)
明确指出零Bugcheck字段也可能来自无法写盘或关机过快，不能排除内核崩溃；
[小转储说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/small-memory-dump)
描述了可用于定位的崩溃线程、调用栈和驱动列表。本轮未修改SD、转储或分页配置，
没有触发人为bugcheck。存储源码仍由其原仓库维护。
