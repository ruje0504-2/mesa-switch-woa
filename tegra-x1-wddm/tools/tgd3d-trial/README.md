# tgd3d ARM64 可还原实机测试包

本包由 tgd3d-vulkan 支线打包。UMD/ICD 使用该支线构建；KMD 固定复用
支线 NATIVE_DESKTOP_158.md 记录的、曾完整任务栏上屏的签名 .158：
源提交 6c854e9f394c3d761fab85885bead4bbd4588581，SYS SHA256
46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088。
所有组件的独立源码提交、长度和 SHA256 见 trial-components.json。
不包含主线 .161 KMD、Zink UMD 或 CPU/风扇/SD 驱动更新。

## 准备与启动

1. 在 Switch Windows ARM64 的管理员终端运行 PREPARE.cmd。
   当前系统必须为已知 .59 / Code 0、原 ACPI、CPU 1497600 kHz / 风扇13。
   工具会复制包到受保护的 ProgramData 目录，导出当前 .59 签名驱动包，
   保存 ACPI、交互桌面显示设置和电源设置，并校验备份。此步不装候选驱动。
2. 进入输出的 PREPARED_DIRECTORY，再运行 START-TEST.cmd。
   自动还原任务先注册，再装 .158，保留启动门控；第一次重启后自动执行
   一次原生启动并进行五分钟 API/截图观察。不会自动运行像素、旋转或 cube 测试。
3. 物理屏幕是否正常必须由人观察。Code 0、ETW Present、截图均不能单独证明
   面板上屏。evidence/native-observation.json 的 API 结果与物理反馈分开记录。

## 还原

- 随时在准备后的目录运行 RESTORE.cmd（管理员），或启动计划任务
  Tgd3dTrialRestore。会撤销本次启动许可和注册，恢复 ACPI，卸载本次 .158，
  安装保存的 .59，重启后恢复并校验显示设置及 CPU/风扇/电源状态。
- 自动回退在启动测试约15分钟或每次开机12分钟后触发；异常重启后的重复启动
  防护也会立即要求回退。保持自动回退任务，测试通过也不会永久安装候选驱动。
- 如果内核完全卡死，软件定时任务无法执行。重启后一次性许可和挥发性门控会
  阻止再次原生启动，恢复任务在系统重新运行后执行。
- recovery-verified.json 的 Verified=true 才代表重启后校验完成。
  如果失败，保留 restore-error.txt 和还原目录，不能把 restore-staged 当作还原成功。

## 证据与范围

保留整个准备目录的 JSON/JSONL、ETL、日志、evidence/ 和 recovery.json。
收集前停止本包 ETW 会话；离线可用 app/tgd3d-trace-export.exe 解码 ETL。
解码器也支持 --follow，使用 tgd3d 独立 provider，不使用 Zink loader 日志判断成功。
已复用的工具来源记录在 tool-provenance.json；打包不会构建主线或合并 main。
