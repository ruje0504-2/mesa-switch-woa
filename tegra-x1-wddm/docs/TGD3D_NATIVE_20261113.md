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
  DWM 的 Present 回压耗时均为0，保留的一次CPU等待被旧工具记为读回，但同一
  wait_sequence 帮助函数也用于销毁，无法据此区分两者；Explorer 有两次
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

## 回退后取证进程耗尽内存

第二轮回退之后，用户又观察到 `.59` 桌面卡住、Explorer 系统错误弹窗，随后
黑色背景只剩图标，部分时刻仅触摸可用。这是另一轮基线故障，不能继续把此前
`recovery-verified=true` 当作系统持续稳定的证明。

用户手动重启后，原生命令取回了 System 2004 事件40390、40394、40398、40413、
40418、40429。它们反复指向 `powershell.exe` PID2408，创建时间
`2026-11-13T06:43:36.1130143Z`。该进程提交内存从4542185472增长到5261848576字节；
事件40413的系统提交量与上限同为6118985728字节。Explorer挂起事件15461、15497
分别记录于07:21:36和07:24:26 UTC。没有取得该弹窗的异常码或分配调用栈。

PowerShell 400事件36963记录同一个PID的完整EncodedCommand。解码后，与本窗口
发起的 `sd-dump-state.ps1` 加固定输出编码前缀逐字一致，SHA256为
`f4dc3dd17b57f0a03593880e1c2c6e5f162dc95cddb4517bedaff56f53b0e56b`。
该临时脚本使用CIM/PnP查询并将注册表PowerShell对象送入JSON序列化；本地SSH
超时后远端进程仍存活，后续取证未及时识别其内存增长。这是本窗口取证流程的
明确问题。没有栈证据，不能进一步断言具体是哪条CIM或序列化语句造成增长。
也不能用这次内存耗尽替代此前两轮原生tgd3d开窗重启的根因。

手动重启启动事件为07:27:40 UTC。07:32与07:44两次进程清单均无PowerShell；
07:44可用物理内存2287132 KiB，可用虚拟内存3002552 KiB。`.59` SYS及ACPI哈希
与原基线一致，风扇13/CPU1497600 kHz、SD0.5.1.2及Wi-Fi0.5.7的实际SYS哈希
均匹配安装记录，显示和睡眠超时仍为AC/DC均0。这是恢复时点的状态核对，
不是新的长期稳定性验收。

后续SD/基线取证改用主机端 `tools/collect_recovery_health.py`：调用原生
reg、pnputil、certutil、wmic OS和限条数的wevtutil；SD只指定
`ACPI\NVDA0101\0`，不再执行上述PowerShell/CIM库存脚本。输出直接写本地文件，
SSH异常或超时记录“远端结束未知”并停止，不继续堆叠取证进程。
六项基线命令在07:44全部退出0，五项SD/转储命令在07:47全部退出0；没有新装
驱动、调频或开启原生测试。旧脚本与原始事件保留在独立产物目录
`hardware/post-recovery-explorer-20261113/`及其相邻的`staged-4309434c/`。

CPU寄存器读回1497600 kHz、OverclockActive=1、Throttled=0、LastError=0；
Windows的CentralProcessor记录仍为998 MHz，与用户照片的任务管理器1.00 GHz
相符。当前SwitchFan直接控制PLLX，不能用Windows的旧频率显示否定硬件读回；
这里没有声称已测得应用性能提升。

事件2004与提交量的解释参考微软
[应用内存泄漏排查](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/troubleshoot-application-service-memory-leaks)
（2026-11-13查阅，适用于Windows Client/Server）：默认工作集列不能替代提交量。
本次没有增加分页文件或改变GPU预算来掩盖取证进程增长。

## 开窗重启：补齐调用进入记录

冻结的早期Zink上屏实现`ee7b0d50`在RuntimePresent前通过
`screen->fence_finish(..., UINT64_MAX)`等待NVK完成；tgd3d使用双向GPU时间线，
只在回压/CPU访问/销毁时等待CPU。这个差异可以形成后续对照，但本轮尚未把
CPU等待改回去，也未认定时间线是根因。两者的着色器前端和ICD也有差异，
不能仅凭旧Zink短时开窗便排除共享资源或编译问题。

此前日志仅记录编译、等待及提交返回，无法识别卡在调用内部的情况。新增
事件14为设备创建阶段（runtime context、VkDevice、共享Fence、command pool、
descriptor layout）；15为DXBC/SPIR-V前端和CreateShadersEXT后端；16为
runtime signal、Vulkan QueueSubmit2和runtime wait。进入为`E_PENDING`，
返回为对应状态，按PID/TID及阶段配对；编号4/30-kind2才是原来的成功设备/
Present依据。时间记录30-kind4是CPU等待进入，kind3仍是结束，并增加原始
VkResult。DESTROY调用现在显式传入原因，旧reason2不能反推它一定是读回。
诊断改变不修改KMD、ICD、时钟、预算、同步算法或NAK编译算法。

2026-11-13 07:58:13 UTC，在`.59 / Code0`运行12项ARM64 CPU控制全部通过，
新增控制令等待返回VK_TIMEOUT或VK_ERROR_DEVICE_LOST，验证未完成的命令和
view仍保留、没有CPU信号伪造完成。实时与离线各14条合成ETW记录字段一致，
3个实时状态记录无丢失，进入/返回及销毁超时分类检查通过。控制包仅含测试和
取证工具，没有加载新的UMD或创建GPU设备。构建曾因没有使用项目已有Python
环境而配置失败，改用已有环境后完成ARM64构建；未修改系统Python配置。

### 旧WATCHDOG转储

10个已取回小转储的头部均为0x141，日期都早于本次tgd3d试验。对最新的
`WATCHDOG-20261112-0204.dmp`，07:57 UTC已用系统自带ARM64 DbgEng成功读取。
实际捕获时间是2026-11-11 18:04:24.860 UTC，系统运行309.031秒；它是旧的
GPU引擎超时live dump。参数2落在`tegragpurender.sys+0x4cfc`，只能说明
报告归属地址，不能当作最后执行指令。

给DbgEng提供匹配系统镜像后可展开12层CPU栈，经过watchdog、dxgkrnl、dxgmms2
和nt；没有匹配PDB，若干名称只是最近导出符号加偏移，不能据此断言具体内部
函数。还读出GUID`270A33FD-3DA6-460D-BA89-3C1BAE21E39B`的19990字节TDR标签。
尚未从中解出当前UMD着色器、GPU命令流或当前两轮硬复位的原因。

读取器`inspect_kernel_dump.cpp`只打开已有转储，不附加活系统；本进程限制
256MiB提交内存并在45秒自行退出，防止SSH断开遗留分析进程。两次受限读取均
正常退出0，第二次解决了第一次缺少镜像unwind表的问题。日志和完整原始标签
保留在独立产物目录`debugger/watchdog-0204-images.stdout`。微软
[0x141说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x141---video-engine-timeout-detected)
说明这是live dump并列出参数与TDR标签；它不等于拿到了GPU指令栈。

## 第三轮：静止通过，隐藏窗口与共享ETW问题

UMD源码`d6470feb38defeb824f34cda39ffbb9e886f611d`，SHA256
`e25c01c90cb63587257baaa67468d480821e41f5e406cea410f220a3db115818`；KMD仍为
冻结`.158`，ICD仍为`7c677212`，各自哈希未变。测试包6721338字节，SHA256
`590f0bd4eec4eeef08a4a54fee0bc2820fa1939a94a2f94eecf73b5a9f9ca04a`。
本轮目录`tgd3d-d6470feb-20261113-160356`，原生启动记录08:07:48 UTC，
LUID`000000000008f379`，DWM PID580、创建FILETIME`134390307694560305`。
静止阶段0/15/30秒检查通过，观察42.93秒；用户确认“画面正常，能操作”。

08:11:03 UTC窗口子进程被启动，PID2576在08:11:06写出Managed Visible=true，
记录8次移动/缩放并于65.03秒退出0。但用户没看到窗口，08:11:33的桌面截图
也没有它。08:12:23观察器因共享会话`events_lost`增加而失败。最初将其说成
“开窗前被工具中止”不正确；后来取回完整动作和进程记录后已更正。

保留ETL有正常EndTime、1365个写出缓冲和1个丢失事件；解码6958个UMD事件，
其中DWM有423次成功Present、429次成功提交。132个着色器返回均成功；
75对设备创建阶段、264对编译阶段、1809对提交阶段的进入/返回数量一致。
保留的两次CPU等待是READBACK，返回VK_SUCCESS；其它错误记录为
DONOTWAIT Map的`0x887b0001`。这不构成实际窗口显示、真实60Hz或完整无丢失验证。

回退首次被清理竞态阻断：停止ETW后，consumer PID1772自行退出；随后
Stop-Process按PID停止时抛出“找不到进程”。08:17:49确认记录的collector都已
退出后重试恢复，08:20:20完成`.59`/原ACPI/显示和平台设置验证，用户确认
“已回到桌面，能操作”。原错误与重试记录均保留。用户报告风扇较快时，
08:15的读回为SoC38C、板温36C、PWM118/236、CPU1497600kHz、未热降频；
这是当时的控制器采样，不是实测RPM或此前峰值温度。

### .59窗口对照与修正

保持原启动参数和隐藏PowerShell父子进程，在`.59`运行同一窗口脚本：
ManagedVisible=true，但IsWindowVisible=false、Iconic=false、Cloaked=0。
它能报告13次移动/缩放并退出0，实际仍未显示，故这些软件计数不能作开窗通过依据。
修正后在Shown回调显式ShowWindow(SW_SHOW)，再检查Win32可见状态并激活自有窗口。
08:26的对照NativeVisible=true、Foreground等于该窗口句柄，90.13秒内17次
移动/缩放并退出0；用户确认“能看到，显示和操作正常”。这是工具在`.59`的
对照通过，不是tgd3d原生窗口通过，也没有修复前两次硬重启。

依据微软[ShowWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow)
（2023-06-06），父进程STARTUPINFO可以覆盖第一次显示命令；后续显式显示才
使用请求状态。[IsWindowVisible](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-iswindowvisible)
（2021-10-13）检查WS_VISIBLE，但仍不能证明未被遮挡或面板正确，因此保留实屏反馈。

另外将UMD独立为64KiB、4至16个缓冲、1秒刷新会话，原内核会话继续保留；
identity读取跳过无关的调用阶段JSON对象构造，原始日志全部保留。回退通过
持有并核对进程对象来停止collector，已退出视为清理成功，真实失败仍报错。
08:31在`.59`通过3项实际进程控制、18项身份读取控制、14条实时/离线合成ETW
传输和脚本语法检查，无GPU工作。原生显示、垂直同步和重启修复仍待下一轮。

本轮证据196文件、130470854字节均已按清单长度/SHA256取回校验。归档20886011
字节，SHA256`0c8d6289eda282f3a6a9de204edf8a8bac100ae4925bee4b0102105ddc447ef7`，
位于独立产物目录`hardware/staged-d6470feb/`。其中原工具、修正后`.59`对照、
两次回退记录分别保留，未覆盖原始失败记录。

## 第四轮：打开快捷设置后冻结，自动回退正常执行

组件仍为`d6470feb38defeb824f34cda39ffbb9e886f611d` UMD、冻结`.158` KMD
和`7c677212` ICD，三个二进制SHA256与第三轮一致；只更新窗口、独立ETW和
collector清理工具至`04034cfd7e6f853070f2d91152dab6c4bce9d439`。包6723706字节，
SHA256 `27ffeb9707a78a248583e4dca702a47c4a2575ea1bd89415b6bd18c7a5b125ed`。
目录为`tgd3d-d6470feb-20261113-164031`；WMI启动时间
08:42:51.647048 UTC，08:45:04原生入口确认`.158 / Code0`。
LUID`00000000000996bd`，DWM PID468/TID1840，创建FILETIME
`134390330044625805`。

静止阶段08:45:22开始，0/15/30秒身份检查通过，但08:46:00的截图调用在15秒后
超时，整段48.39秒未通过。用户报告点开右下角Wi-Fi/快捷设置时画面冻结，并指出
首次原生输出时曾正常打开此面板；具体触摸时刻未单独记录。因此这轮包含交互，
不能标成纯静止桌面偶发冻结，也不能仅凭一次触发判定稳定复现。没有请求Window
阶段，没有新的彩色窗口、五分钟交互或性能程序回归通过结果。

冻结时08:46:29 SSH仍能执行hostname，实时UMD日志在恢复前取回。完整独立
UMD ETL正常结束，无丢事件/缓冲，2193条载荷与实时日志逐条相同。DWM保留15次
成功提交和Present，最后完成值14；80对设备阶段、336对编译阶段、198对提交
阶段进入/返回数量相符。26条错误均为DONOTWAIT Map的`0x887b0001`，没有保留
CPU等待开始/结束记录。DWM不再产生日志后，其他进程仍继续；这些结果不能证明
最后一帧已扫描完成，也不能证明GPU全局挂起或供电故障。

最后的DWM资源记录为1280×13、bind0x28。审查发现旧事件8在CreateResource
**入口**便写S_OK，尚未执行分配和绑定；不能把它解释为资源创建已经成功返回。
内核记录最后的paging fence7313，在另一条System工作线程上有相同对象的
UnwaitCpuWaiter，间隔0.2005ms，随后paging packet312结束。因此不能把这条
主线程等待当作已证实的Fence超时。内核/UMD墙钟偏移仍需保留各自时钟，不能
直接跨ETL按FILETIME减法推断调用顺序。下一步需要DWM用户栈与分阶段创建记录。

08:46:41恢复脚本完成`.59` staging，System1074 **Record40865**确认SYSTEM
通过shutdown.exe发起计划重启。08:47:11手动恢复SSH请求未连接成功，未送达；
不能把此次重启记作又一次无预兆硬复位。无新的41/6008。08:48:48恢复校验通过，
`.59 / Code0`、原ACPI/显示和平台设置保持，用户确认“已回到桌面，能操作”。

证据157文件、108256188字节全部按长度和SHA256取回核验；归档11813391字节，
SHA256 `1094b801e67e2722f242d3a1c33f4587fada164b732e94b1d9c472d3d675fd66`。
位于独立产物`hardware/staged-04034cfd/`，物理反馈、后补系统事件和分析分别
保存。原始内核228480条记录中，按保留的本机模板解码170178条DxgKrnl记录，
无该provider的未知/损坏载荷。Wi-Fi触发及偶发性目前都是待验证条件，不是根因。

### 冻结调用栈取证

新工具使用PSS快照和系统DbgHelp，只写线程上下文、栈及模块信息，不写完整堆。
通过PID、创建FILETIME和镜像名核对同一DWM，先保留进程句柄；错身份不抓取。
写入器自身限制256MiB提交内存、20秒取消/25秒退出和32MiB输出，成功刷新后
才从`.pending`发布为`.dmp`。捕获失败记录原因并继续恢复，不改变保护策略。
恢复开始时先保存KMD Parameters原始文本，再抓DWM；卸载后才读注册表会丢失
本轮状态。每轮已有取证收据则不覆盖或重抓。

09:17:56 UTC在`.59`完成初步控制：错创建时间被拒绝；自建目标成功捕获后正常
退出0；实际DWM PID560的快照455856字节，捕获468ms，写入器峰值提交1941504
字节。系统DbgEng可展开19个线程；没有PDB的内部名称仍只能看成导出名加偏移。
这只验证取证能力，不是冻结时栈，更不是重启修复通过。

实现依据Windows SDK `10.0.26100.0`的minidump头文件及微软
[MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump)
（2024-02-21）、[PSS快照导出](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/proc_snap/export-a-process-snapshot-to-a-file)
（2018-05-31）和[PssCaptureSnapshot](https://learn.microsoft.com/en-us/windows/win32/api/processsnapshot/nf-processsnapshot-psscapturesnapshot)
（2025-10-31），均于2026-11-13查阅。适用本机Windows 11 ARM64；采用示例的
snapshot callback契约，但省去handle表和全内存，只保留本次DWM停顿定位所需数据。
不依赖旧WATCHDOG dump，不保证掉电或完全内核锁死时还能执行用户态取证。

09:24:08 UTC，正式构建产物在`.59 / Code0`通过12项CPU合约、18项读取判定、
16条真实ETW实时/离线传输和全部脚本语法检查。取证写入器的错身份拒绝、目标
正常退出、恢复入口抓DWM以及已有收据不覆盖均通过。该入口本次生成465088
字节DWM快照，312ms，写入器峰值提交1908736字节；`.59`没有原生KMD Parameters，
注册表读取退出1如实保留。没有安装候选或执行GPU验收。

新UMD SHA256 `58a26cd6c8f2d6d17c13d5e9b90bb13b64759ee34eab59b73de11e8e74ae4af9`，
写入器 `50be687f0df6a9ba8876974e0ed963ab67d978f39c420faa5b63c5ebef604bb5`。
本轮只补资源调用进入/返回和失败取证；绘制/同步算法、KMD与ICD未变，冻结根因
尚未定位。控制证据归档1644793字节，SHA256
`73a79b1ee360c3fd1ef37736822b4ed7de14d8ee2bce42051dd077bbf689ac7a`，位于
独立产物`resource-phase-controls/`。后续原生试验仍须物理观察及失败即回退。

### 末次渲染完成与冻结后的VBlank

继续按内核对象和提交序号配对：DWM的NVK context `0xffff9906adb0cdd0`
提交36对应DMA751，有177/176完成和180队列结束记录，未超时/抢占；随后同一
context提交37的时间线15 signal结束。runtime context `0xffff9906ae3addd0`
等待同一fence对象`0xffffd3040d4ee820`的值15（提交61）亦结束，Flip提交62及
后续信号63/64/65均结束。故UMD末条Present中completed=14只是当时采样，
不能据此认定序号15永远未完成。冻结`.158`源码`TgGpuSignalContextWork`在
通知DMA完成前检查GPU marker、GPU/CPU完成及退休状态；本次仍只把ETW作为
内核完成报告，不据此替代像素或物理桌面验收。

通过同一内核记录的FrameQPCTime和UMD原始QPC作近似关联，可将末次纹理入口
与后续驻留/两次GPU VA映射放在同一段时间，而不直接混用两份ETL的FILETIME。
这一对齐存在通知延迟，不能当成微秒精度的函数进入/返回。paging fence7313
的0.2005ms仍来自同一内核时钟的wait/unwait配对，与跨会话校准无关。

该原生适配器在末次资源入口后另有4396条VSyncDPC，跨度73.25秒；周期中位数
16.66651ms，范围16.64594至16.68682ms，报告的扫描地址始终为4223320064。
冻结源码`panel_interrupt`仅在Tegra DC的VBlank状态位有效时发通知，先读取
锁存状态，再报告Displayed.Address。因此证据表明显示中断仍在运行、上报地址
未变化；不能把“约60Hz中断”写成画面持续更新、无撕裂或完整VSync通过。
没有据此改KMD、提高CPU频率或加入CPU等待替代GPU Fence。

回调边界再次核对微软[SetVidPnSourceAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress)
（2022-11-04）及[DxgkCbNotifyInterrupt](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkcb_notify_interrupt)
（2021-10-27），2026-11-13查阅；原始配对和时钟样本保留在
`hardware/staged-04034cfd/analysis/last-render-completion.json`和相邻分析文件。

下一轮冻结包源码为`94cc811acb056d0fbe9374c510a55c0dbe389be3`，6937619字节，
SHA256 `1b6f96ae70b92148dacb31fcf2052da7ef64027237154677580349140529cd16`。
正式UMD SHA256 `2f908f4e0f11bae47f093cf3c0ece7bf86cd94ce829d6349e219def490c0b1cf`；
复构建使PE时间戳变化，CPU合约与取证程序已逐字节确认只有PE/Debug目录时间戳
不同，代码和数据相同，详见`resource-phase-umd-arm64/control-equivalence.json`。
设备只完成备份/准备，尚未启动这轮原生测试；仍需新的物理观察回复。

## 第五轮结果与源码对照方向

上述`94cc811a`包随后实际运行：同一冻结`.158`和`7c677212` ICD，DWM PID464、
LUID`0000000000096c84`。09:55:23至09:56:06 UTC静止阶段完成，用户确认画面正常、
能操作。09:58:54设备记录Window请求；用户随后报告“没有出现就重启”。本轮失败，
不能记开窗通过。10:03:56 UTC系统恢复校验确认`.59 / Code0`和平台设置保持，用户
确认桌面正常、能操作。此前已取回的174文件归档SHA256为
`9f790c55b1abae6ef1ded72429d3378c38195307e1ebf94b19b342c96be7cee8`。
保留的UMD/内核末尾早于开窗请求约51/53秒，不能据此定位故障瞬间。

用户要求停止继续查新日志/Dump，改为直接对比源码、一次改变一个变量并上机。
本轮不再扩展取证工具；恢复脚本不再先抓DWM Dump。

2026-11-13源码对比以实际历史交付为基准：Zink UMD来源
`833c4423ec6ed668a257d3896fabf26ff5e9f1ad`、ICD来源
`8c361754f3059cec0ecacb5543e25adf7eba51b2`；tgd3d审查基线`2e5d1313`、ICD
`7c677212e5843725f3dbb12ee0b9dad6b1339a05`，KMD仍冻结于
`6c854e9f394c3d761fab85885bead4bbd4588581`。没有合入main或改KMD。

| 路径 | Zink桌面UMD | tgd3d审查基线 | 结论范围 |
|---|---|---|---|
| 普通Flush/Present | `RuntimeDeviceFinish`等待Gallium fence完成 | runtime signal → Vulkan wait/render/signal → runtime wait | 有明确同步差异；未从源码证明环路 |
| 多设备 | Zink按物理设备复用VkDevice及queue lock | 每个D3D Device独立创建VkDevice及KMT上下文 | 新设备/上下文压力不同；未发现已证实的句柄越界 |
| 共享布局 | 72字节元数据、128字节pitch、4行补齐 | 同一格式/大小协议，另核对Vulkan rowPitch/offset/requirements | 未发现明确布局越界；不能代替已知像素验收 |
| 编译 | SHDR→TGSI→NIR→Zink SPIR-V→NVK/NAK | 重建DXBC→dxbc-spirv→NVK/NAK，shader objects | NAK源码与目标SM53相同；输入/绑定/动态状态不同，不能排除特定输入的编译缺陷 |

核对的微软契约包括[DDI线程规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)、
[GPU wait callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_waitforsynchronizationobjectfromgpucb)
与[signal callback](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_signalsynchronizationobjectfromgpucb)
（后两者2024-02-22）、[共享资源创建](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createresource)、
[像素着色器创建](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createpixelshader)
（2021-04-01）。tgd3d只协商D3D10.0 DDI且不声明free-threading；不能套用已启用
D3D11 free-threading的宽松条件。此KMD声明WDDM2.6；其monitored fence等待由
调度器控制，不能当成[WDDM3.2 native GPU fence](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/native-gpu-fence-objects)。

静态审查另发现两个失败路径问题，尚未修改或宣判为本轮根因：
`wait_cpu`到期仅设置tgd3d局部lost，资源/设备销毁随后调用无期限`DeviceWaitIdle`；
`create_shader`在失败时没有析构已经保存的tokens/signatures，而微软明确创建报错
后不会调用DestroyShader。前者需要保持在途对象安全，不能简单删除等待后释放；
后者属于失败清理问题，不能据此声称解释了成功提交时的硬件重启。

### 仅关闭跨队列桥接的临时对照

提交`d06e29e223b9ba22e85b58bf49eaca6fa457ec31`提供显式构建参数
`build_tgd3d.py --cpu-sync-control`，默认构建仍保留GPU桥接。对照版不创建两个
runtime monitored fence、不导入Vulkan，也不排runtime↔NVK的GPU wait/signal。
本地Vulkan完成时间线和相邻提交依赖保留；普通Flush/Present/共享释放改为CPU
等待真实完成，仿照旧桌面UMD。DONOTWAIT/Query轮询仍只提交、不做CPU等待。
这是诊断构建，不是永久退回同步渲染或宣布同步问题已修复。

ARM64/x64构建完成；x64 Wine下12项CPU控制通过，包括原生标志下不创建/导入
跨上下文fence、普通Flush完成、轮询不阻塞和超时不伪造完成。三份实际转换的
VS/PS/采样SPIR-V通过`spirv-val --target-env vulkan1.3`；这不验证SM53机器码执行。
可还原包6936555字节，SHA256
`603a778caea98ca15cbfe37b9895f8ffc961f18852a1841623ccb40119704152`。
仅UMD同步方式改变，KMD/ICD仍为上述冻结文件。实机结果另记，不由控制测试推断。

`.59`上ARM64同一控制程序亦通过12项，全部PowerShell脚本解析通过；已准备
`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-d06e29e2-20261113-185158`，备份和恢复
校验完成，尚未安装启动。UMD SHA256
`b3ba7de9bb07a5627ddf907fd7e6a07d1a2fe33dcab977d06f41ba0adb450ffc`。
Antigravity CLI的Carbon High独立源码审计已启动；其结论须经本窗口复核。

第一次同步对照在开窗前被UMD消费者状态校验拦下，自动恢复；不能记为同步模式
开窗失败。11:01:35 UTC恢复校验通过`.59`。源码确认消费者无事件时不刷新状态，
而身份读取器强制要求10秒内状态；`a877d211`仅对显式CPU对照允许静止状态，仍
要求同一DWM创建/Present及LUID一致、无丢事件。22项读取控制通过；GPU二进制
不变。该工具修正后的包已准备但尚未启动时，用户进一步修改恢复策略。

**最新用户指令：以后直接以冻结`.158`为基线，不再自动退`.59`。** 新恢复动作
保留`.158`及匹配ACPI，撤销临时tgd3d/ICD注册和启动许可；重启后使用关闭门控的
`.158 / Code43`及BasicDisplay可操作桌面，再核对原显示、CPU1497600及风扇13。
`.59`只保留归档备份。历史记录中的`.59`回退要求不再适用于新试验。

### 同步对照开窗结果及进一步的用户纠正

工具`4f3b7a2e`、UMD`d06e29e2`的CPU同步对照于11:13:55 UTC启动，KMD、ICD及
UMD二进制均保持上述冻结哈希。11:17:13–11:17:57的静止阶段约39.12秒，
11:19:26–11:20:44的Window阶段约72.54秒，均通过API检查，同一原生LUID
`000000000008c1a6`。窗口实际存活45.22秒、移动8次、缩放8次，退出0；用户确认
“现在窗口出来了”。未确认全部物理运动/像素，也没有五分钟稳定性通过。

该对照同时移除了runtime↔NVK桥接并使普通Flush等待真实GPU完成，因此只能
缩小到同步/并发及其影响的资源生命周期，不能单凭一次结果宣判特定Fence死锁。
默认关闭桥接的临时编辑已按用户纠正撤回；正式构建仍默认启用GPU桥接，
`--cpu-sync-control`只用于诊断。下一版需要针对源码中的具体缺陷修复。

主会话在Window结束后错误地继续按试验流程触发恢复重启。11:24:28保留`.158`
及匹配ACPI，11:26:36校验`.158 / Code43`、显示和平台设置保持；这不是继续运行
原生桌面。用户明确纠正：**留在当前`.158`开发，不要测试结束就回退/重启。**
该指令取代上一段“自动关闭门控再重启”的解释。源码已删除自动恢复定时任务和
阶段错误时自动恢复，保留显式手动恢复工具。已确认设备上旧Restore/Automatic
任务不存在，且没有待执行关机；没有再次发出重启。测试结果及用户原话保存在
独立产物`hardware/cpu-sync-d06e29e2-idle/`。

Carbon High的tgd3d审计和DXVK后续任务按用户要求停止，均返回interrupted；
审计尚未给出最终报告。输出和DXVK三个未提交文件的补丁已保存在
`gemini-takeover-20261113/`，不作为已验收结论合入。随后用户要求恢复审计，
11:30:37 UTC通过Antigravity CLI恢复同一Carbon High会话，补充上述对照结果，
保持只读并要求给出具体缺陷及最小修复；DXVK后续任务仍停止。

### 统一对照条件，复用当前.158启动

进一步核对本轮已完成的身份结果：Static-30的TraceState间隔26.103秒，
Window-0为3.404秒，Window-60为11.152秒。旧检查对GPU桥接模式仍强制10秒，
但CPU对照允许静止，因此工具条件不一致；不能把这轮差异全部归因于Fence。
`Assert-NativeDesktop`现在对两种提交模式都接受同一当前进程/适配器的静止身份，
仍校验LUID、进程创建时间、消费者存活、成功创建/Present及事件损失。此身份
检查不证明画面持续更新，窗口动作和物理反馈仍需分别验收。

冻结KMD的`start_device`每次调用`TgGpuConsumeNativeStartPermit`，并非只在
DriverEntry读取许可。对已确认`.158 / Code43`、SYS/ACPI匹配的当前启动，
测试入口改为直接观察、授予一次许可并PnP重启显卡设备；不重装KMD、不重启
Windows、不改CPU/风扇。原生已经启用时仍直接继续阶段，不再次启用。
11:45:58 UTC，在实机`.158 / Code43`解析26份脚本并通过22项读取控制，尚未
启用本次GPU对照。GPU桥接默认、KMD/ICD及渲染代码均未改变。
