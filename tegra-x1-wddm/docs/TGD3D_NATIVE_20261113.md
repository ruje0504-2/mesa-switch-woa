# tgd3d 首轮原生桌面与异常重启

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
