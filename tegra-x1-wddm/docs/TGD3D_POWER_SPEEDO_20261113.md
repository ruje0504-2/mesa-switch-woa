# tgd3d 电源、Speedo 与候选频率

更新：2026-11-13（本机时区；SSH采样为2026-11-14 UTC）。本轮仅读取、计算和提出
方案，没有修改实机频率、电压、限流、风扇、睡眠或驱动。当前组件与登录后启用流程
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

下一轮应先做单独降低GPU频率的可回退对照，并保持其他组件一致；在同频、同电量范围
比较AC/DC后，再判断是否需要检查PMIC限流/供电路径。降低频率成功也只能缩小范围，
不能单独排除时序相关的软件错误。动态调压和空闲降频是后续节能方向，无须先移植
整个Linux DVFS框架才能做固定低频对照。
