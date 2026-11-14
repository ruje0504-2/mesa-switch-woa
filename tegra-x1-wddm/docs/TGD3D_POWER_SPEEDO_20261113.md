# tgd3d 电源、Speedo 与候选频率

更新：2026-11-13（本机时区；SSH采样为2026-11-14 UTC）。最初审查仅读取、计算和提出
方案；随后用户指定GPU460.8MHz、内存1600MHz、CPU不变，安装状态见文末。
当前组件与登录后启用流程
见 [冷启动交接](TGD3D_COLD_BOOT_HANDOFF_20261113.md)。

## 实际芯片校准值

来源为用户提供的 Hekate「硬件和Fuses信息」照片
`72217185483346bff6914f5fa2603364.jpg`，不是SSH直接读取。照片本体留在本地。

* SoC：T210B01 / Mariko，M2 A01；SKU `0x83`，Hoag / Vali，零售版。
* DRAM ID19：Samsung K4U6E3S4AA-MGCL，4GB。

| 项目 | CPU | GPU | SoC |
|---|---:|---:|---:|
| Speedo实际值 | 1630 | 1659 | 1675 |
| Hekate显示的IDDQ校准值 | 360 | 545 | 332 |
| 固定Switchroot源码推导的Process ID | 0 | 1 | 1 |
| SKU默认Speedo ID（无启动参数覆盖） | 0 | 1 | 0 |

IDDQ是芯片出厂校准数据，不能用作本轮GPU负载电流。Speedo用于选择电压曲线，
不单独证明超频上限、板级供电瞬态或系统稳定性。该版本Process ID阈值：CPU1650，
GPU1584/1695，SoC1598/1709；区间判断为严格小于。GPU Speedo存于CPU_SPEEDO_2，
不是照片中单独列出的CPU Speedo1（1371）；SoC Speedo2为1466，也不替代SoC1675。

分档依据：[speedo-tegra210.c](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/soc/tegra/fuse/speedo-tegra210.c)。
Hekate显示依据：[gui_info.c](https://github.com/CTCaer/hekate/blob/8e1392df4569ac501a115867056b13c361429e9e/nyx/nyx_gui/frontend/gui_info.c)。

## 当前Windows状态

SSH只读记录位于仓库外 `power-review-20261113/live-ac/`；02:30:24 UTC电源API报告
ACLineStatus=1、电量96%、正在充电。当前没有同负载电池供电对照，也没有电压跌落波形。

| 项目 | 核对结果 | 证据边界 |
|---|---|---|
| CPU | 请求及实际1497600kHz，CpuMv1120，DriverVersion13 | 频率寄存器与PMIC设定回读；不是模拟电压瞬态测量 |
| RAM | 请求2100000kHz，实际1866000kHz，HandoffStatus8 | 8为RateMismatch；不能写成实跑2100MHz |
| GPU | 冻结.158渲染初始化固定729600kHz | 来自已安装组件对应源码；当前接口未导出实测频率/电压 |
| PMIC | 地址0x33；CoreMv1050 | 0x33为MAX77812 phase211；CoreMv不等于GPU或DRAM电压 |
| 温度 | SoC42°C、板38°C、SensorFailures0 | 采样时的数值，不能代表重启瞬间 |
| SD | SDR104，199680kHz，4bit、1.8V信号、调谐成功 | DDR200未启用；ADMA错误0，历史CMD5错误1，不能据此判供电故障 |

平衡电源计划的CPU最小/最大比例均为AC/DC 5%/100%。差异包括无线节能AC0/DC2、
媒体/JavaScript策略；SwitchFan13直接配置PLLX，不能由Windows百分比推断实际降频。
任务XML允许电池供电启动且不因切换电池停止，不能把任务策略当作已证明的AC/DC根因。

冻结GPU实现先读Speedo，再设置M1电压并回读，等待5ms后启动NA PLL；渲染目标为
729.6MHz，借用768MHz CVB包络。它没有运行期DVFS，运行期间保持idle slowdown关闭。
计算范围为-40..105°C、800mV下限、5mV向上对齐，超过1050mV拒绝启动。
它没有统一设置PMIC的限流/强制PWM字段，不能从Hekate头文件中的注释猜测当前限流值。

## 这台机器的参考电压计算

固定参考版本：Switchroot维护的NVIDIA 4.9源码
`2d0059fd3167a8df756de2aa0489d4aa70a9fc15`。没有证明SD内Ubuntu镜像与该版本相同。
CPU选非SLT p4v3，GPU选非SLT NA p4v3；未选AggressiveSLT/HighOPT，也未覆盖Speedo ID。
保留整数逐步四舍五入及5mV向上对齐，计算结果在仓库外
`power-review-20261113/device-speedo-cvb.json`。

| CPU参考档位 MHz | 按Speedo1630计算的DFLL OPP mV |
|---:|---:|
| 1581 | 845 |
| 1683 | 895 |
| 1785 | 950 |
| 1887 | 1005 |
| 1963.5 | 1055 |
| 2014.5 | 1085 |
| 2193 | 1125 |

这是DFLL的OPP曲线参考，不是当前Windows PLLX可直接写入的电压表。CPU的PLL备用
路径另有CVB_PLL_MARGIN=30%的计算；不能跳过时钟源区别，把845mV直接用于Windows，
也不能把表中的1120/1235mV上限当作所有频率的通用工作电压。当前1497.6MHz保留
1120mV设定，具备进一步核对功耗的价值，但没有执行降压。

依据：[CPU DFLL表](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/clk/tegra/clk-tegra124-dfll-fcpu.c)、
[CVB运算](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/soc/tegra/cvb.c)。

GPU Speedo1659代入完整c0..c5温度公式，沿用当前固定档的800mV下限：

| GPU档位 MHz | -40..105°C公式最大值 mV（上取整显示） | 加800mV下限后的固定候选 mV |
|---:|---:|---:|
| 384 | 610 | 800 |
| 614.4 | 616 | 800 |
| 768（也覆盖当前729.6） | 670 | 800 |
| 844.8 | 689 | 800 |
| 921.6 | 712 | 800 |
| 998.4 | 746 | 800 |

800mV是所用固定策略的下限，不是各频率公式都恰好得到800mV。该计算支持
“当前729.6MHz设定未低于此参考包络”，不证明真实供电没有跌落，也不证明998.4MHz
已经可以稳定运行。本轮没有读取运行中M1设定，也没有GPU压力测试。
温度扫描范围沿用现有实现，不表示允许设备工作到105°C。

依据：[GPU CVB及800mV备用下限](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/soc/tegra/tegra210-dvfs.c)。
低压运行还受到EMC频率、工艺分档和热策略约束；本轮不采用610mV裸系数作推荐值。

## 候选方案，均未部署

“通用”范围限定同代Mariko设备，并要求对应主板的PMIC/时钟/温控实现正确；不适用于
Erista。对未逐机校准的设备，首选低频率，不提高限流或强行套用本机高频参数。

| 方案 | CPU MHz | GPU MHz | 内存 MHz | 电压策略与适用范围 |
|---|---:|---:|---:|---|
| 通用起步 | 1497.6 | 384 | 1600 | 本Lite固定CPU1120mV、GPU800mV作为保守候选；别机沿用匹配主板的已验证CPU策略，原厂内存电压 |
| 本机日常候选 | 1497.6 | 614.4 | 先1600 | 保留CPU1120mV，GPU800mV；先确认电池负载稳定 |
| 本机性能候选 | 1996.8 | 768 | 先1600 | CPU需PLLX适用性验证；GPU参考800mV，内存1866另轮验证 |
| 本机上限探索 | 2188.8 | 998.4 | 先1600 | 2193MHz DFLL参考1125mV不可直接套PLLX；尚无已验证CPU电压，不列入默认包 |

1996.8/2188.8MHz来自当前38.4MHz、M=2的PLLX频率步长；它们不是Ubuntu的
2014.5/2193MHz DFLL档。高频候选只是研究目标，不能作为可安装的完整设置表。
当前GPU源码只接受729600和1228800kHz，新增上述档位仍需实现及验证，不能仅改注册表。

Hekate L4T配置默认VDD2=1100mV、VDDQ=600mV可作Mariko内存电压参考，未取得当前
实机两条内存电压轨的读数；不更改原有内存设置。保留512MiB预算和风扇13。

## SD与后续电源判断

Hekate在卡支持时通过CMD6将UHS功率档切至1.44W；更高档代码被禁用。该值是协议
选择，不是测得的电流。Windows当前SD主控的200/400/600/800mA能力位也不是实测。
1.8V是UHS信号电压，不能据此把卡的VDD供电当成1.8V；目前没有支持“提高SD限流
即可修复GPU重启”的证据。

参考：[Hekate SD](https://github.com/CTCaer/hekate/blob/8e1392df4569ac501a115867056b13c361429e9e/bdk/storage/sdmmc.c)、
[MAX77812布局](https://github.com/CTCaer/hekate/blob/8e1392df4569ac501a115867056b13c361429e9e/bdk/power/max77812.h)、
[L4T配置](https://github.com/CTCaer/hekate/blob/8e1392df4569ac501a115867056b13c361429e9e/README.md)。

采用可回退频率对照，并在同频、同电量范围
比较AC/DC后，再判断是否需要检查PMIC限流/供电路径。降低频率成功也只能缩小范围，
不能单独排除时序相关的软件错误。动态调压和空闲降频是后续节能方向，无须先移植
整个Linux DVFS框架才能做固定低频对照。

## 用户指定的频率组合与当前进度

后续反馈修正：插电时也偶发重启，只是比电池供电复现较少；两种供电状态均未通过
稳定性验收。用户选择同时降低GPU和内存，并自行进行桌面及后续冷启动测试。
该组合用于稳定性比较，不能单独区分GPU降频和内存降频的贡献。

| 项目 | 用户选择 | 应用前实际状态 | 本轮状态 |
|---|---:|---:|---|
| GPU | 460.8MHz，800mV | 固定729.6MHz路径 | 硬件计数器460800kHz，启动电压设定回读800000µV |
| 内存 | 1600MHz，电压保持 | 实际1866MHz、请求2100MHz | 重启后请求/实际均1600，HandoffStatus=1 |
| CPU | 保持1497.6MHz、1120mV | 1497.6MHz、1120mV | 重启后读回相同，风扇13 |
| 风扇／预算 | 驱动13／512MiB | 同左 | 保持 |
| 启动方式 | 先登录后tgd3d，再冷启动 | 登录后5秒触发 | 初始测试保留登录后启用 |

GPU实现提交 `68d9502b435c5563fa11f32949b045589111b171`；使用N=48、P=2，
使VCO保持1843.2MHz，输出GPC 460.8MHz。默认源码配置仍为729.6MHz，新包显式传
`--render-gpu-khz 460800`。ASan/UBSan时钟模型、ARM64构建均通过；Windows硬件入口
控制中旧实现退出117、新实现退出0。它们不执行真实GPU。Inf2Cat无错误/警告，13个
目录成员通过Windows签名检查，仍不代表桌面验收通过。

已安装SYS SHA256：`59dd5ea77415a9ef474e8d8bc5a8cae86e74713d3c0ab534ae0ad683dacf6e81`。
恢复SYS：`c629328e7309381aa1b9923576ab57a4e664102a393d385ecf5643113641dd53`。
UMD `ad5e3309…`、ICD `dee3d580…`及固件字节保持不变；原384MHz候选未安装。

安装完成于2026-11-14 03:14:03 UTC，随后03:14:40–03:15:17执行登录后启用。
03:16:00的校验记录显示`.158 / Code0`、HardwareStatus/PanelStatus均0，DWM PID496
加载`TegraGpuD3DVk.dll`，CPU/RAM/GPU读回与上表一致。用户确认了前一阶段门控桌面
可操作；本次原生频率组合的桌面交互稳定性仍等待用户测试，冷启动尚未进行。
03:16:55的只读CCD/KMT查询确认源、目标及原生适配器LUID均为
`00000000000cfc6d`，matching_kmd/nvk_ready均为1，当前显示仍720×1280。
冻结查询工具退出4是缺少可选probe导出后的预期结果，两个查询记录成功；没有因此
运行额外D3D设备/绘制测试，也没有把退出4记作GPU失败。

实机安装目录：`C:\ProgramData\SwitchWOA\GpuUpdate\tgd3d-42ad5d1b-20261114-111337`。
`RESTORE.cmd`恢复729.6MHz的bit14 KMD并保持门控；内存旧INI另存为
`woa-lite-home.before.ini`，不会在GPU恢复时擅自覆盖用户选定的1600MHz。
原启动INI SHA256 `2c656430ad3da51aea2db87e0337b693d8db2b122ce1144e859a5e736b50aded`；
新INI `bbf1f989bf52cb81b398e5909480157ae54af928660d5645bfce86fb9e3f9691`。
本轮只为内存生效重启了一次，KMD在门控状态安装后启用，没有对活动GPU热换驱动。
源码中的冷启动工具已允许同一KMD在桌面测试后配置下一次原生启动，但本轮尚未设置
`AllowNativeBoot`；登录后5秒触发任务保留。

内存配置属于 [winx](https://github.com/ruje0504-2/winx/commit/6bc267b0671b5382acaf8fdde109162c37efeff7)：
仅把 `woa-lite-home.ini` 的 `ram_oc=2100000` 改为 `1600000`，原 `cpu_oc=1996800`
启动请求不变，Windows运行期仍由SwitchFan13维持1497.6MHz。现有固件已支持1600MHz，
无需替换UEFI。Windows系统驱动仓库本轮没有代码变更。

电池侧另有一项待核对的集成差异：实机SwitchBattery报告版本11，充电策略状态0、
REG05为0x8A；BAT0只提供两个I2C连接。对应源码仅在第三个PD连接存在时启用USB-C
协商/限流管理，所以当前Windows的该管理分支没有接通。它不能证明充电芯片或PD
控制器没有自主工作，也不是已定位的重启原因。本轮未改电池驱动或PD配置。
