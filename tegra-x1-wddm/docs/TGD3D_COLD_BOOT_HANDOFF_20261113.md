# tgd3d 冷启动重启：探索记录与后续交接

更新：2026-11-13（本机时区；交付回执使用 UTC）。范围为本聊天最近的原生桌面、
登录前重启及 Deferred Init 对照。本文供用户后续安排 Claude 审查；本轮未派发给 Claude。

## 当前结论与恢复状态

**GPU460.8MHz＋RAM1600MHz 的直接 tgd3d 冷启动仍自动重启，按用户反馈记为失败。**
该次失败仍使用 CPU 同步对照 UMD `ad5e3309…`，不能归到本轮随后准备的双向 Fence 版。
用户要求恢复“进入系统后再启用 tgd3d”，随后追加要求把双向 Fence 也写入，由用户测试。
SD已于 `2026-11-14T03:45:51.538472+00:00` 完成写入和实卡读回：74个文件哈希通过，
SYSTEM语义差异仅为删除AllowNativeBoot，SOFTWARE仅增加所需RunOnce。
后续登录引导、DWM实际加载和实屏稳定性仍待用户启动验证。

用户反复观察到：**接外部电源时，桌面天气模块和浏览器相对更稳定，但也会偶发重启；
电池供电时更容易复现。** 没有同负载、同电量、同观察时长的受控AC/DC比较，不能
给出改善百分比，也不能认定供电、PMIC、时钟或Fence中的某项已经是根因。
这条现象及低频冷启动失败一并交给后续审查，不能写成“插电已稳定”。

本轮已写入的配置与验收边界：

* 保留 `.158 + bit14 + GPU460.8MHz` KMD、`d3423d26` ICD、RAM1600MHz；
  CPU1497600kHz/1120mV、风扇13、睡眠设置与512MiB GPU预算保持。
* 删除持久 `AllowNativeBoot`，首次登录由 `!SwitchWOA.Tgd3d.LogonSetup` RunOnce
  调用已有 `Enable-LogonStart.ps1 -StartThisSession`，注册 SYSTEM 的
  `Tgd3dDefaultEnable` 登录任务，触发延迟 `PT5S`。首次RunOnce执行时间取决于
  Windows调度；5秒是触发延迟，不是GPU启用已经完成的承诺。
* UMD从冻结 `9de42c82` 加 XRGB `7f5f416b`、资源错误传播 `42ad5d1b` 出发，
  仅追加已审查的 `a537121f` Flush错误传播修正，去掉CPU同步控制宏，启用现有
  runtime→NVK与NVK→runtime的双向GPU Fence。没有恢复Deferred Init或混入shader缓存。
* ARM64、x64全部31个tgd3d对象分别重编；x64/Wine控制通过双向提交顺序、失败类别、
  DONOTWAIT、超时不伪造完成及在途资源不提前退休等检查。它们不执行真实GPU，
  新组合的物理桌面、五分钟稳定性、冷启动与双向Fence实机结果均未验收。
* 普通提交使用GPU依赖；两帧队列回压以及显式读回仍可能有CPU等待。不得称为
  “所有CPU等待均已消除”或“重启根因已修复”。软超时后的无界DeviceWaitIdle仍未解决。
* 保留原CPU同步UMD、匹配清单和控制程序的离线恢复包
  `D:\SwitchWOA\gpu460-logon-cpu-sync-backup.zip`；只恢复同步组合时保留登录后启用、
  GPU460.8和RAM1600。扇区备份对应更早的失败冷启动配置，不作为首选同步回退包。
* 未连接Switch SSH、未额外收集崩溃日志、未安排Claude/Gemini或新一轮实机自动测试。
  该交付时由用户测试并等待Claude安排；后续用户已授权继续硬件旋转和SSH自主测试，见文末。

频率的最后运行期读回为2026-11-14 03:16 UTC：GPU460800kHz/800000µV、
RAM1600000kHz（Ready=1）、CPU1497600kHz/1120mV；Code0且DWM加载tgd3d。
03:24:35 UTC曾按用户要求设为持久原生冷启动，随后用户报告仍自动重启。
这些旧读回不能代替本轮双向Fence组合启动后的实际状态。
电源/Speedo及频率记录见 [电源审查记录](TGD3D_POWER_SPEEDO_20261113.md)。

本次保留已有冷启动ETW/WER，按原约定在用户确认修复后清理。
既有 `EnableLUA=0`、计划任务私有TaskCache和显示配置不作改动。
注册语义参考微软 [LogonTrigger.Delay](https://learn.microsoft.com/en-us/windows/win32/taskschd/logontrigger-delay)
和 [Run/RunOnce](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)，
查阅2026-11-13。离线阶段只放置引导，任务重新注册由下一次登录后的Windows完成。

此前Deferred Init撤回提交为`a019010d`，生产源码恢复到实验前`4c8691cb`；
登录引导工具为`f71cd0bd`。上一轮恢复后用户确认约30秒桌面正常，后续天气/浏览器
仍有重启。该轮02:11:31–02:12:06 UTC启用，SSH曾核对Registered/PT5S/Enabled，
DWM PID828和Explorer实际加载tgd3d；不能把该旧短时结果移用于本轮。没有合并main。

## 固定组件与源码边界

系统：Windows ARM64，23H2，build **22631.2861**；不能把构建SDK26100或维护WinPE
22621.5337当作实机系统版本。每次启动LUID重新读取，历史LUID不复用。

| 组件 | 源码来源 | 实际 SHA256 |
|---|---|---|
| 保留的729.6MHz恢复KMD | 冻结 `6c854e9f394c3d761fab85885bead4bbd4588581`，叠加 bit14 `253bc813e43a878359b9d6227e246cd7d1aa382f`、持久许可能力 `b916f53105f991294d6811718362e90f9af0ba01` | `c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53` |
| 当前`.158` GPU460.8 KMD | 上述冻结基线、bit14和持久许可能力，加时钟对照 `68d9502b435c5563fa11f32949b045589111b171` | `59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81` |
| 本轮双向Fence tgd3d UMD | 冻结 `9de42c82` + `7f5f416b` + `42ad5d1b` + `a537121f6b9d8448a18a7bfe1ddc1d1c5332b493`；GPU-bridge构建 | `14d51a5526dccd00737e15bef0f3bd7a40b38535a47c9083184d62ad7d1eb6c4` |
| NVK ICD | `d3423d269f9e716eadc1237beadbfb6aa6049dbe` | `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0` |
| 保留的CPU同步对照UMD | 冻结 `9de42c82a03e84674954d8175237dcfd5bb4dc2e`，加 XRGB `7f5f416bc785e0ac9cc687c1ae6369e9355deed1`、资源错误传播 `42ad5d1bfa95c73466b7f76378f28792522742ec`，CPU-sync构建 | `ad5e3309f1d5e8b7e549d70e5a5741da1ee6cf7c4e4f49782e0067c7b858b3f7` |
| 已撤回 Deferred Init UMD | 上述冻结组合加 `5e029182`、`a7a60d29`；全部tgd3d对象重编 | `9f195e43dbd5f5fa19e569c919cf39f67512f9b9b8cba4c432a5c3f3602bbae9` |

**硬件包不是整个HEAD快照。** 当前开发源码中的 shader binary cache不在本轮冻结UMD内。
本轮显式带入`a537121f`并构建GPU桥，之前冷启动失败包仍是CPU同步对照。
仅完成编译和离线安装不能写成生产双向Fence实机通过。

固定源码导出分别位于输出目录的：
`kmd158-gpu460/source/tegra-x1-wddm`、
`gpu460-bidirectional-candidate/source/src/nouveau/tgd3d`、
`resource-status-candidate/source/src/nouveau/tgd3d`；
已撤回实验源码位于 `deferred-init-candidate/source/src/nouveau/tgd3d`。

## 已做过的主要轮次

| 轮次／修改 | 代码或控制结论 | 实机结果与范围 |
|---|---|---|
| 早期 `3c5d327d` | DWM实际使用tgd3d，保留251次成功Present | 用户确认短时桌面可用，打开CPU/风扇/温度程序后重启；不是稳定性通过 |
| 关闭跨队列GPU桥的同步对照 | 保留本地Vulkan timeline，Flush/Present CPU等待 | 有开窗成功，随后天气/网页关闭仍有失败；不是双向Fence已修好 |
| `fcc53bf1` UMD错误处理 | 修正设备丢失通知及失败对象生命周期 | 原 `.158`、旧ICD组合通过30秒、60秒和300.262秒交互，用户确认；随后Home导致Explorer重启，整体稳定性仍失败 |
| `d3423d26` ICD | NVK meta保存/恢复公共shader绑定标记；旧控制会丢VS，新控制保留VS | 从门控状态启动后Home正常；天气仍重启。该轮仍是原SYS，不能归因于后加的bit14 |
| `253bc813` bit14 | 在golden context保存前清除 `0x419e44` bit14并读回；保留其余异常位 | 用户指定作为固定比较基线，尚无同组合完整稳定性结论 |
| 持久原生启动 | 一次性许可不适合持续开机；`b916f531`增加显式持久许可 | 直接开机仍异常重启；许可改动不等于修正电源或绘制流程 |
| `9de42c82` | 支持标准primary/shadow OpenResource，不强制私有TGSR描述 | 冷启动失败；UMD SHA `6612e45e8059086e9fe8dfb7b1f9c1cd868bed78e19722da0aff9ce90d26fa9c` |
| `7f5f416b` | 补齐合法XRGB主表面，保留原格式与布局；真实DDI CPU控制旧失败/新通过 | 冷启动失败；UMD SHA `cfeab491db032ed6903a2d5f5bdb0c50dde89e9f7d17241564e52b908b52a6bc` |
| `42ad5d1b` | 保留OOM/移除类别，正确清理失败对象；资源/上传控制通过 | 冷启动仍失败；保留为本轮UMD的资源修正基础，不把它叫作已修复冷启动 |
| 并发冷启动捕获 | 配置DWM、LogonUI局部WER与4路ETW | 未得到可归因的进程Dump；旧流补全发现见下一节 |
| 实机系统shader的CPU编译复现 | 28份独立SM4程序均通过tgd3d→SPIR-V、Vulkan1.3校验及NVK/NAK SM53编译 | 无GPU执行/像素验收；未覆盖运行时链接库与实际故障shader |
| GPU460.8＋RAM1600，CPU同步UMD | 频率读回正确，保持CPU1497.6、bit14和修复ICD | 用户反馈直接冷启动仍自动重启，失败；接电相对更稳定但未通过稳定性验收 |
| 本轮GPU双向Fence＋登录后启用 | 冻结UMD加已审查Flush错误传播修正，完整重编及CPU控制通过 | 等待用户实机测试；不能复用旧组合通过结果 |
| Deferred Init | 布局/NAK后端延到首次使用；桌面事件后建立GPU桥；双输出完成检查及失败清理控制通过 | 用户反馈仍重启，已撤回。未取得新版实际加载证据，不能据此排除全部初始化/同步问题 |

五分钟通过的精确历史组合：原SYS
`46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088`，
fcc53bf1 UMD `7c7f88ccf83ad1a89f663c3205e6ba2cb6fec587d946efd3622839c9deb2b089`，
旧ICD `17dc3110393365eef279a90104850d6f517c74659441916266afeae0ae2c8f73`。
它不能代替当前组合验收。详见 [完整轮次记录](TGD3D_NATIVE_20261113.md)。

## 新取回材料：旧冷启动流得到补全

这次取回的4个 `.etl.001` 都逐字节包含此前 round1 文件的完整前缀；这是旧流的
补全，不能仅按本次取卡时间将它标作 Deferred Init 的新日志。只对已知固定布局
解码tgd3d记录；原始文件没有改写。ETL头的 EndTime/BuffersWritten仍为0，尾部不完整。

* 22个物理缓冲内有989条有效自定义事件：DWM PID792为800条，LogonUI PID784为189条。
* 两进程各有成功CreateDevice，包含原有5个创建阶段；有32次shader后端完成记录。
* 成功Present记录为DWM 71次、LogonUI 5次；记录中的完成值分别达到80和7。
  这是调用/完成值证据，不代替物理画面与稳定性验收。
* 两条 `0x887b0001` 来自 staging Map 的等待分支，是本驱动的
  `DXGI_DDI_ERR_WASSTILLDRAWING`，不能把它当作已证明的Fence超时。
* 没有Deferred Init事件19；kernel Image_Load记录的UMD映像尺寸为2445312字节，
  与实验前尺寸相同，与撤回候选2449408字节不同。尺寸不是完整文件哈希，
  但足以说明这些记录不能证明 `a7a60d29` 已加载。
* 本次取卡时保存的Autologger FileCounter已到2，而上述已取回流为 `.001`；后续应优先
  核对Windows可见的 `.002`，避免把NTFS事务尚未回放时看到的目录当作完整记录。

旧流现在能证明DWM与LogonUI都进入了tgd3d并提交/呈现，修正此前“持久化前缀尚
没有DWM记录”的资料状态。仍无具体故障栈、超时返回或最后GPU指令，不能宣判
缺DLL、Mutex死锁、非法SM53指令、供电或时钟中的某项已定位。

证据：`deferred-init-rollback/cold-boot-artifacts.json`、
`analysis/umd-events.jsonl`、`analysis/loaded-umd-identities.json`、
`analysis/cold-stream-findings.json`。快速启动和休眠的取卡时注册表值均为0，
没有因上述旧流现象擅自修改电源配置。

## 已明确的修正与尚未证明的因果

1. **Home/Explorer的CPU访问违规已有具体缺陷。** 旧ICD异常位于
   `nvk_cmd_flush_gfx_shaders+0x358`，访问空的 `last_vtgm`。meta恢复遗漏
   `pipeline_shader_stages`，PS-only更新会丢VS；d3423d26修正该状态保存/恢复。
   这与整机冷启动重启应分别讨论。
2. **所谓46506字节问题不是直接改一条SM53指令。** NETB该偏移存放寄存器
   `0x419f78` 的地址字段，值在后8字节；冻结 `.158` 已在golden save前清bit3，
   `0x10beb→0x10be3`，并检查读回。bit14是另一个寄存器
   `0x419e44`，`0x00d3eff2→0x00d3aff2`。后续context restore后的实读仍未取得。
3. **风扇桥接在启动阶段已有路径。** KMD通过 `\Device\SwitchBacklight` 与
   SwitchFan13的内核IOCTL通道做PMIC/时钟等操作；不依赖桌面温度程序先运行。
   历史ETL中switchfan早于GPU驱动加载约4.6秒。尚无负载瞬态电压/转速测量。
4. **Fence仍有未解决责任。** 保留的CPU对照没有完整runtime→NVK ingress；本轮GPU桥
   必须单独验证。软超时后的无界 `DeviceWaitIdle` 仍未解决，不能直接删除等待并
   释放在途对象。旧全局quarantine方案的VkInstance/ICD/运行时allocation寿命不完整。
5. **着色器静态编译通过不保证硬件执行正确。** Windows独立SM4测试为18VS、10PS；
   最大1215条指令、64GPR。没有捕获实际失败shader及其完整绑定资源、描述符和上下文。

来源与适用条件见 [GR helper/bit14说明](GR_HELPER_LOADS.md)、
[微软DDI线程规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/changes-from-direct3d-10)、
[Vulkan同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)。
比较 Windows 后端时使用 `nvkmd/windows`，不能把 `__SWITCH__` 分支当作已在Windows生效。

## 工具问题也必须保留在交接中

* `desktop-path.exe`退出4是缺少可选私有导出，已修正验收分类，不算GPU失败。
* 曾有测试窗口未真正显示；修正ShowWindow/交互会话调用后才取得可见窗口对照。
* `.59`上的另一轮桌面异常与本窗口取证进程有关：事件2004指向
  `sd-dump-state.ps1`所属PowerShell PID2408，提交量增长到5.26GB，系统提交耗尽。
  SSH超时后没有及时识别远端进程仍在运行，是本窗口流程错误；没有栈证据定位到
  具体CIM/序列化语句，不能据此认定tgd3d资源泄漏。后续改为限量原生命令采集。
* 强制复位曾留下零填充状态JSON和不匹配恢复副本，已引入WriteThrough、Flush与
  恢复集校验。恢复时使用当前SD的新维护副本，不覆盖Windows运行前的旧扇区快照。
* 当前Mac NTFS只读；实际写入复用Windows WinPE原生NTFS检查和COW预演，逐段备份、
  写回、读回。文件系统检查通过不解释GPU重启。

## 等待用户安排 Claude 的检查入口

用户另行安排后再提供本报告、冻结源码与交付清单；当前不派发审查，不并行操作设备。
请先从实际组合出发，而不是审整个HEAD后默认所有改动已部署：

1. 对比登录后启用与直接冷启动的DWM/LogonUI并发、NVK/runtime上下文、GPU VA及
   共享allocation生命周期。旧流证明两进程都曾提交，但尚无根因。
2. 分开审查默认双向桥和实际CPU对照。给出具体依赖链及失败后寿命，不能靠
   CPU伪造完成、提前退休或默认屏蔽同步来宣称修复。
3. 从真实DDI资源入口检查标准primary/shadow、pitch/offset、XRGB/ARGB及跨进程
   导入；注意已有9de/7f5/42ad修正，避免再次提交同一修正作为新根因。
4. Shader审查覆盖dxbc-spirv映射、NVK公共绑定状态、NAK SM53及实际资源布局。
   静态可接受和GPU语义正确分别报告；保留具体来源提交。
5. 首先核对 `.002` 的会话、模块尺寸/版本和启动身份。报告确定缺陷与因果假说，
   给可回退diff及最小必要控制；不要将旧ETL前缀、API成功或CPU控制包装为实屏通过。

Carbon曾返回修订审计，bit14建议经本窗口独立核对后采用；另一次冷启动审计会话
在503/EOF等错误后停止，没有交付所要求的report/diff，不能标作完成审计。
未将未验收的第三方diff合入当前硬件包。

## 硬件旋转接续点

用户后续已授权继续旋转并恢复SSH，要求尽量自主完成测试；本轮进度见文末。
冻结 `.158` 已含DC列扫描、H/V取址、几何锁存与VidPN/MPO映射，尚无该组合实屏
横向验收。tgd3d四分之一转主表面已有1280×720、5120字节pitch处理；
`tgd3d_dxgi.cpp::Blt` 的非identity旋转仍返回不支持，是需要核查的明确边界。

依据固定Switchroot `3c40adf1081096bc61a7c1280434c163e7e79b48` 的
[window.c](https://gitlab.com/switchroot/kernel/l4t-kernel-nvidia/-/blob/3c40adf1081096bc61a7c1280434c163e7e79b48/drivers/video/tegra/dc/window.c)
和微软 [旋转契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)
核对列扫描、预缩放维度、末字节反向起点及不同接口的方向约定。
用四角异色、箭头、文字和一像素边框检查真实扫描地址、方向、裁剪与鼠标坐标，
通过再保存方向设置，不能只改Windows方向标志。

用户之前要求的shader缓存与实际60Hz/VBlank时序仍需完成对应组合验证；冻结包
尚未带入缓存，SyncInterval=1仅是请求。当前也没有已知像素、横屏五分钟或vkcube
通过结果。不得将旋转、缓存、同步及KMD多个变量一次性混装来代替逐项验证。

## 本机交付位置

根目录：`/Users/rujian/Documents/Codex/mesa-switch-woa-tgd3d-vulkan-out`。

| 内容 | 相对输出目录 |
|---|---|
| 本轮恢复计划、实卡回执、注册表差异和CPU同步备份 | `kmd158-gpu460/logon-restore/` |
| 本轮双向Fence源码、完整构建命令/哈希与控制结果 | `gpu460-bidirectional-candidate/` |
| 上一轮Deferred Init恢复及备份 | `deferred-init-rollback/deploy/` |
| 用户的延迟初始化失败反馈 | `deferred-init-candidate/physical-result.json` |
| 撤回实验的源码、命令、对象/二进制哈希及CPU控制 | `deferred-init-candidate/build-manifest.json`、`source/` |
| 旧冻结资源修正组合 | `resource-status-candidate/build-manifest.json`、`source/` |
| 冷启动捕获设置与初次记录 | `cold-boot-concurrent-20261113/` |
| 本次取回的旧流补全及固定结构解码 | `deferred-init-rollback/cold-boot-artifacts/`、`analysis/` |
| 真实Windows独立shader的编译控制 | `dwm-shader-audit/summary.json`及逐shader结果 |

Windows原始shader及派生代码留在本机，未加入Git；后续交接按实际需要传递材料。

## 双向Fence之后的横屏模式协商续查

用户随后要求推进硬件旋转，并明确允许连接SSH测试。2026-11-14 03:54:22 UTC
读回当前UMD `14d51a55…`、KMD `59dd5ea7…`，启动模式logon-default，状态Enabled，
HardwareStatus/PanelStatus均0、GPU460800kHz/800000µV；DWM PID840、Explorer PID1568
和系统UI进程均加载tgd3d。后续PnP查询为Code0。活动源、目标、原生KMT的LUID均为
`0000000000091038`；交互桌面Default/session1，720×1280、60Hz、identity。
03:57 UTC电源API为AC=1、电量99%。这些是当前运行状态，仍不构成长期稳定性验收。

现有包内display-state是旧工具，rotation-test首次退出2，只表示命令不支持，未改模式。
从当前源码重新编译工具后，90°严格CCD校验返回1610。随后只读对照确认：

| 请求角度 | 严格CCD | 允许调整模式细节 | 省略目标模式 | 源/目标均由OS选择 | GDI CDS_TEST |
|---|---:|---:|---:|---:|---:|
| 0° | 1610 | 0 | 1610 | 0 | -2 |
| 90° | 1610 | 1610 | 1610 | 1610 | -2 |
| 180° | 1610 | 0 | 1610 | 0 | -2 |
| 270° | 1610 | 1610 | 1610 | 1610 | -2 |

整个查询前后都保持原720×1280，没有强行应用失败配置或请求重启。
KMT真实模式列表仅两项：720×1280/XRGB/60Hz，orientation=1和3；
ValidatedAgainstMonitorCaps=1，ModePruningReason=7。**7在这里是驱动推荐、面板接受的
原因，不是模式被裁掉的错误码。** 推荐目标也为720×1280、78MHz、total1000×1300。
GDI普通模式列表为空，raw/rotated列表只有800×600占位模式，不能据此说面板只支持800×600。

本轮候选仅修正KMD模式枚举：保持未旋转源模式720×1280/2880 pitch，允许该模式
对应四个旋转角度，并保留已固定的旋转支持约束；移除将它与CCD横屏桌面尺寸混为
一谈、排除90°/270°的条件。实际DXGI主表面仍通过48字节描述明确提交自身
1280×720/5120 pitch等布局，allocation和扫描控制代码未变。

固定`.158+bit14+GPU460.8`源码上仅叠加panel_vidpn.c及匹配控制测试，候选SYS SHA256：
`609ce9d9e119aace6a29cb04b7ada290aa0cffacc4f9db898f433b5c24d7a4c4`。
主机构建和模型通过；同一新VidPN断言链接旧实现退出170、修正后退出0，扫描/布局
控制退出0。Inf2Cat无错误/警告，13个签名目录成员经Windows验证通过。
这些检查不执行真实GPU；后续安装及失败结果见下一节。
恢复包保留当前`59dd5ea7…`的460.8MHz KMD，不回到729.6MHz或`.59`。

本轮实际参考：微软[旋转契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-rotation-in-a-display-miniport-driver)、
[模式协商约束](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/enumerating-cofunctional-vidpn-source-and-target-modes)、
[SetDisplayConfig及ALLOW_CHANGES](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig)、
[模式支持原因](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmdt_mode_pruning_reason)，
查阅2026-11-13；WDK/SDK为10.0.26100.0，实机仍22631.2861。
Windows-driver-samples/KMDOD的main文件于同日核对，仅参考模式接口；其CPU显示实现
不作为GPU实现移植来源。Switchroot固定`3c40adf1081096bc61a7c1280434c163e7e79b48`
的window.c用于核对列扫描、反向起点和VBlank更新规则，未复制Linux代码。
控制与原始查询保存在仓库外 `rotation-bidirectional-20261113/`。

## 临时横屏失败及恢复（2026-11-14 UTC）

04:23:33 候选 `609ce9d9…` 启用为 Code0，UMD仍 `14d51a55…`、ICD仍
`dee3d580…`，双向Fence开启。原生LUID `00000000000dc272`，Win11 22631.2861。
KMT由两个方向变为四个方向，`SDC_ALLOW_CHANGES`下0/90/180/270均能校验；
严格CCD仍返回1610。04:32:54起竖屏图案控制运行约30秒，逻辑截图四块色样及完整
白边共5020个RGB像素、容差0、差异0，程序退出0。没有物理画面验收。

04:34:54的第一次90°请求实际上已被Windows接受，但工具把CCD源尺寸与逻辑桌面
尺寸混为一谈，错误地返回13；该轮立即恢复竖屏成功。这是工具误判，不能算GPU失败。
工具改为分别核对native CCD源720×1280、横屏DEVMODE1280×720及原始DSI时序。

04:37:57 第二次临时90°应用返回0，读回DEVMODE1280×720、CCD源/目标720×1280、
path rotation=2，注册表仍portrait。用户随后报告自动重启；新启动时间为04:38:20。
最后落盘阶段为TemporaryLandscape，没有彩色图案完成记录，**本轮实机失败**。
不能据此确定DC已正确旋转、GPU已经完成，也不能确定根因是Fence、供电或扫描布局。
未保存永久横屏，未执行五分钟横屏或vkcube。

04:43:02请求一次恢复所需的重启，关闭测试启动许可；04:45:04核对Code43门控关闭。
恢复到原GPU460.8 KMD `59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81`，
保留双向Fence UMD `14d51a5526dccd00737e15bef0f3bd7a40b38535a47c9083184d62ad7d1eb6c4`
及ICD `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0`。
04:46:50恢复Code0和登录后5秒启用流程；DWM PID780及Explorer加载tgd3d，
GPU读回460800kHz/800000µV，HardwareStatus/PanelStatus均0。
没有回退`.59`、没有恢复高GPU频率、没有关闭双向Fence。
04:48:13–04:49:15恢复后的60秒竖屏图案控制退出0，5020个逻辑RGB像素全部匹配，
前后Code0。用户当时离开，实屏仍未验收。

继续静态核对固定Switchroot `3c40adf1` 的dc_config.c/window.c/ext/dev.c：
T210 Display A Window A声明列扫描及packed rotation支持；列扫描尺寸交换、末字节
反向偏移及VBlank几何更新与当前实现相符。这不能证明Windows上的实际寄存器锁存。
微软SetVidPnSourceAddress及旋转契约也已核对，未据此擅自改动供电、Fence或多项KMD路径。

后续发现UMD创建/打开路径不一致：创建主表面及KMD分配描述支持四方向，
无UMD资源payload的OpenResource却硬编码identity/720×1280/2880。
契约测试已在旧 `14d51a55…` 对应对象上复现失败；修正按分配描述验证旋转、尺寸、
pitch，保持系统标准shared primary/shadow的未旋转契约，不重新分配或旋转像素。
冻结源码只叠加该修正；x64/ARM64构建和x64 CPU契约通过。
这是一处确定的源码不一致，尚无证据证明失败的90°轮实际经过该拒绝分支；不写作已修复重启。
