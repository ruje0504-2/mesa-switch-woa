# tgd3d 冷启动重启：探索记录与后续交接

更新：2026-11-13（本机时区；交付回执使用 UTC）。范围为本聊天最近的原生桌面、
登录前重启及 Deferred Init 对照。本文为用户后续交给 fable 准备，尚未派发给 fable。

## 当前结论与恢复状态

**直接原生冷启动仍未解决；Deferred Init 未取得成功结果，已按用户要求撤回。**
当前恢复为：保留 `.158 + bit14 + 修复 ICD`，使用实验前的 CPU 同步对照 UMD，
登录后触发原有 tgd3d 启用流程。用户已反馈能够进入桌面并持续操作约30秒；
后续 SSH 已核对登录引导 Registered、任务触发延迟 PT5S、启用状态 Enabled，
GPU门控/初始化错误字段均为0，DWM PID828及Explorer等进程加载了
`TegraGpuD3DVk.dll`。启用记录为2026-11-14 02:11:31–02:12:06 UTC；
当前原生LUID与完整加载路径尚未另行采集。用户随后仍反馈天气/浏览器导致重启，
并指出插电时似乎正常；恢复启动流程已确认，整体稳定性仍未通过。
电源与实际Speedo核对见 [电源审查记录](TGD3D_POWER_SPEEDO_20261113.md)。

恢复动作已在实卡读回核对：

* UMD恢复为下表 `ad5e3309…`；移除实验的 `tgd3d-desktop-ready.exe` 和专用 Run 值。
* 删除 `ControlSet001\Services\tegragpurender\AllowNativeBoot`，避免登录前启动原生GPU。
* 首次登录用 `RunOnce` 引导注册标准 SYSTEM 任务 `Tgd3dDefaultEnable`；后续登录
  采用 `AtLogOn`、`PT5S`。复用现有 `Default-Start.ps1` 的许可、PnP启用和状态检查。
* 首次引导执行时间由 Windows 调度，不能承诺恰好登录后第5秒已完成启用。
  后续任务的5秒是触发延迟，组件校验和GPU初始化另计。
* KMD/ICD二进制、CPU1497600kHz、风扇13、睡眠设置及512MiB GPU预算未变。
  当前已有 `EnableLUA=0`；本次未修改UAC或私有TaskCache。
* 保留已有冷启动ETW/WER；仅在用户确认修复后清理。

注册语义参考微软 [LogonTrigger.Delay](https://learn.microsoft.com/en-us/windows/win32/taskschd/logontrigger-delay)
和 [Run/RunOnce](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)，
查阅2026-11-13。离线阶段只放置引导，实际任务注册要由恢复后的Windows完成。

源码撤回提交：`a019010d`。生产代码、Meson选项和 tgd3d 测试已逐文件比对，
恢复到实验前 `4c8691cb`。登录引导工具提交：`f71cd0bd`。没有合并main。

## 固定组件与源码边界

系统：Windows ARM64，23H2，build **22631.2861**；不能把构建SDK26100或维护WinPE
22621.5337当作实机系统版本。每次启动LUID重新读取，历史LUID不复用。

| 组件 | 源码来源 | 实际 SHA256 |
|---|---|---|
| `.158` KMD | 冻结 `6c854e9f394c3d761fab85885bead4bbd4588581`，叠加 bit14 `253bc813e43a878359b9d6227e246cd7d1aa382f`、持久许可能力 `b916f53105f991294d6811718362e90f9af0ba01` | `c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53` |
| NVK ICD | `d3423d269f9e716eadc1237beadbfb6aa6049dbe` | `dee3d5805af80696f87d094ec1c5f557d662f435336207992baf588e2dc1cee0` |
| 恢复的 tgd3d UMD | 冻结 `9de42c82a03e84674954d8175237dcfd5bb4dc2e`，加 XRGB `7f5f416bc785e0ac9cc687c1ae6369e9355deed1`、资源错误传播 `42ad5d1bfa95c73466b7f76378f28792522742ec`，CPU-sync构建 | `ad5e3309f1d5e8b7e549d70e5a5741da1ee6cf7c4e4f49782e0067c7b858b3f7` |
| 已撤回 Deferred Init UMD | 上述冻结组合加 `5e029182`、`a7a60d29`；全部tgd3d对象重编 | `9f195e43dbd5f5fa19e569c919cf39f67512f9b9b8cba4c432a5c3f3602bbae9` |

**硬件包不是整个HEAD快照。** 当前开发源码中已有的 shader binary cache、部分
Flush改动不在恢复的冻结UMD内。源码默认启用GPU桥；实机工作包是CPU同步诊断模式。
不能将这个包的结果写成生产双向Fence通过。

固定源码导出分别位于输出目录的：
`kmd158-bit14-native-boot/source/tegra-x1-wddm`、
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
| `42ad5d1b` | 保留OOM/移除类别，正确清理失败对象；资源/上传控制通过 | 冷启动仍失败；这是当前恢复的UMD，不把它叫作已修复冷启动 |
| 并发冷启动捕获 | 配置DWM、LogonUI局部WER与4路ETW | 未得到可归因的进程Dump；旧流补全发现见下一节 |
| 实机系统shader的CPU编译复现 | 28份独立SM4程序均通过tgd3d→SPIR-V、Vulkan1.3校验及NVK/NAK SM53编译 | 无GPU执行/像素验收；未覆盖运行时链接库与实际故障shader |
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
4. **Fence仍有未解决责任。** 当前CPU对照没有完整runtime→NVK ingress；默认GPU桥
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

## 后续给 fable 的检查入口

用户另行发起交接时，先提供本报告、冻结源码与交付清单；本窗口仍统一操作设备。
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

按用户最新顺序，先核对登录后tgd3d已真正启用，再推进正常手持方向1280×720。
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
| 最新恢复计划、实卡回执、注册表差异和备份 | `deferred-init-rollback/deploy/` |
| 用户的延迟初始化失败反馈 | `deferred-init-candidate/physical-result.json` |
| 撤回实验的源码、命令、对象/二进制哈希及CPU控制 | `deferred-init-candidate/build-manifest.json`、`source/` |
| 旧冻结资源修正组合 | `resource-status-candidate/build-manifest.json`、`source/` |
| 冷启动捕获设置与初次记录 | `cold-boot-concurrent-20261113/` |
| 本次取回的旧流补全及固定结构解码 | `deferred-init-rollback/cold-boot-artifacts/`、`analysis/` |
| 真实Windows独立shader的编译控制 | `dwm-shader-audit/summary.json`及逐shader结果 |

Windows原始shader及派生代码留在本机，未加入Git；后续交接按实际需要传递材料。
