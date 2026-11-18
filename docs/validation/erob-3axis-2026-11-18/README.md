# 三轴 eRob 实物联调，2026-11-18

三台 eRob90H100T-BM-18CN 已通过 can0、1 Mbit/s、10 ms CSP 功能联调。主站通过 rocos-shm v2 接收目标，实际关节反馈与独立 SocketCAN 抓包一致。采用逐轴使能，每轴等待 700 ms；三轴同时使能曾触发欠压。本记录是三轴功能证据，不代表原 M5 六轴、8 ms 台架验收通过。

## 运行环境与当前状态

- 源码基线 `ab923db0d2525f234d868cb19fe8fdf70ea9fde9`，产品版本 `0.0.0-rc.6`。本次新增配置、实物测试客户端及抓包分析参数支持，未改主站运行核心、OD、第三方代码或 ROCOS-App，本次以 `0.0.0-rc.7` 归档；rc.7 仅做离线构建验证，实物运动证据对应上述 rc.6 主站。
- Ubuntu 22.04，Linux `6.8.1-1064-realtime`，USB 驱动 `kvaser_usb_kcan`。接口实际位速率 1000000，sample point 0.750，txqueuelen 1000。
- 主站使用 Debug 二进制、普通 `SCHED_OTHER` 调度、继承 CPU 亲和性；实测二进制摘要见 [binary-sha256.txt](binary-sha256.txt)。测试客户端在实测后补充了 int32 目标上界前置检查并重新编译；[交付二进制摘要](delivery-binary-sha256.txt)单独保存，主站二进制未变化。
- 初次联调结束时主站 PID 为 **15327**，SHM ID **2**，Node-ID **127**。三轴控制字为 0、状态字为 `0x0240`，均已失能，模式显示为 8。抓包进程已停止。
- [最终接口统计](final-interface.txt)：CAN ERROR-ACTIVE（正常错误活动状态），bus-off、收发错误及丢包计数均为 0。

## 配置和手册依据

部署文件为 [config/erob_3axis/co.yaml](../../../config/erob_3axis/co.yaml)，本目录的 [co.yaml](co.yaml) 是测试时快照。三轴地址为 1、2、3，SHM 槽为 0、1、2。三台读回 vendor `0x5A65726F`、product `0x26483052`、revision `0x00020111`、固件 `150403171`；序列号对象均为 0，不能据此唯一识别实物。

参考提供的《eRob CANopen and EtherCAT 用户手册 V1.11》印刷页 31、39–44、138、140–141、174，以及《eRob机器人关节模组用户手册 V3.46》印刷页 3、63、65。另核对了提供的英文版通信手册。型号 BM 表示带制动器、多圈编码器；18 是通孔直径，输出编码器为 19 bit，位置单位为 524288 count/输出轴一圈，不能再次乘减速比。

主站 SYNC 周期为 10000 us。0x60C2 在手册中为保留、不支持，未写入；从站 0x1006 保持原值 0，该对象按手册仅适用于 SYNC 生产者。实际从站 0x1005 为 0x80，不生产 SYNC，依照主站 SYNC 更新同步 PDO。

每轴 RPDO1 为 6040/16 bit、607A/32 bit、6060/8 bit；TPDO1 为 6041/16 bit、6064/32 bit，TPDO2 为 606C/32 bit、6061/8 bit，传输类型均为 1，其他 PDO 关闭。合计 SHM Input 33 B、Output 21 B。主从生产心跳均为 100 ms，双向消费超时均为 300 ms。见 [配置检查](config-check.txt)、[初始读回](initial-readback.json)和[最终完整 PDO 读回](final-readback.json)。

## 实测结果

最终轨迹为三轴同时执行 6 s 平滑余弦往返，峰值目标增量分别为 4096、6144、8192 count（2.8125°、4.21875°、5.625°）。使能前将目标对齐实时反馈，逐轴使能后各等待 700 ms，再开始轨迹。回到起点后保持 1 s，再正常失能。

| Node-ID | 实际最大位移/count | 最大跟随误差/count | 回程保持时误差/count |
| --- | ---: | ---: | ---: |
| 1 | 4098 | 29 | -1 |
| 2 | 6148 | 47 | -1 |
| 3 | 8194 | 58 | -5 |

以上误差来自 SHM 在周期通知后的采样，与当时 SHM 目标比较，包含周期延迟，不是驱动内部跟随误差对象 0x60F4。制动器接合后位置会有数 count 的变化；表中回程误差为失能之前的保持阶段。最终三轴 0x603F 均为 0，状态为失能。

- [最终运动记录](final-motion.txt)通过全部判据；[逐周期 CSV](final-motion.csv)含状态字、位置、速度、模式、目标和控制字。
- 1049 次采样、共 3147 条三轴记录，与独立抓包中对应的 9441 个 PDO 帧逐字段比较，**零差异**。同时检查了 SHM ABI、后端、槽数、字节数、字段索引、宽度、偏移、实例和在线状态。见 [SHM/CAN 核对](shm-can-audit.json)。
- 最终测试覆盖 1048 个闭合 SYNC 窗口，**缺失窗口 0、相位错误 0**；SYNC 间隔 min/avg/max 为 **9.421 / 9.999988 / 10.501 ms**，相对 10 ms 的最大绝对偏差 **0.579 ms**。六帧反馈最迟 **0.755 ms** 收齐，最后目标帧到下一 SYNC 的最小余量 **4.383 ms**。见 [时序摘要](final-timing.json)及[窗口表](final-windows.csv)。
- 较长运行中的 SHM 周期统计 min/avg/max 为 **7.5341 / 10.0000 / 12.4783 ms**。该数值包括本次诊断全程，不能只用最终短窗口的抖动代表长期最坏值。见 [交付前检查](final-inspect.txt)。
- `cmake --preset debug`、`cmake --build --preset debug` 完成，[CTest 8/8 通过](ctest.txt)。离线测试与本次真实硬件结果分开保存。

时间统计来自本机 SocketCAN 时间戳和主机 SHM 发布时间；未测驱动内部锁存时刻、独立机械停稳时间或六轴共同锁存，不据此承诺硬实时上限。

## 发现与处理

1. 初始三轴 0x603F 为 `0x730F`（多圈电池低压），0x1001 为 0，状态字未置故障位。手册说明电池低压时仍可使能，但断电多圈保持不可靠。本次采用当前反馈基准，未重置编码器、修改零位或屏蔽电池保护；恢复长期多圈使用前需要处理电池并确认零位。
2. PREOP 维护会话退出后曾读到 `0x8130` 心跳错误。主站恢复持续 100 ms 心跳后执行一次标准 CiA402 fault reset，后续运行未再出现该错误。未改变 300 ms 消费超时。
3. 三轴同时使能约 110 ms 后产生 `0x3220` 欠压，运动轨迹尚未开始，客户端立即撤销使能。失能后母线约 48.1 V，保护下限 44 V、上限 55 V。用户确认电源功率可能不足并要求逐轴使能。改成逐轴 700 ms 间隔后成功，采样母线约 47.95–48.15 V；**未降低欠压阈值**。见 [电压读回](voltage-readback.json)、[分轴电压采集](stagger-voltage.csv)。此结果不证明当前电源足以承担三轴高负载运动。
4. 固件在控制字 7 下使能后，约 300 ms 从 operation enabled 直接进入 switch on disabled（0x0240），未保持 switched on（0x0233）。实物客户端接受这两个正常失能结果，等待制动器接合，再写控制字 0 并确认失能。首次轨迹已完成但旧判据报超时的记录保留在 [stagger-motion.txt](stagger-motion.txt)，最终复验完整通过。

## 复现

本次主站现已停止。重新运行时只启动一个同 SHM ID、同总线的主站。退出时向实际主站 PID 发送 SIGINT，由主站执行组停机流程；PID 15327 是本次历史实例，不能用于后续会话。

```bash
cmake --preset debug
cmake --build --preset debug
cmake --build --preset debug --target erob_csp_bench
ctest --preset debug --output-on-failure
./build/debug/bin/rocos_co_master --check-config --config config/erob_3axis/co.yaml

# 主站没有运行时启动；接口由部署侧配置为 1 Mbit/s 并置 UP。
./build/debug/bin/rocos_co_master --config config/erob_3axis/co.yaml

# 另一个终端。inspect 读取 PDO，消费槽 8 通知；不能与其他槽 8 客户端并用。
./build/debug/bin/erob_csp_bench 2 inspect build/inspect.csv
# 仅在本次已准备台架上执行，确保没有其他 SHM 输出写入者。
./build/debug/bin/erob_csp_bench 2 move build/motion.csv
```

测试客户端不会自动复位故障。若启动时已有故障，需要先读取故障原因、排除原因并显式复位，然后再运行。此工具不加入自动 CTest，也没有修改 App 行为。

原始独立抓包为 [run1-can.log.gz](run1-can.log.gz)。解压后可复算最终窗口（索引包含此前诊断过程）：

```bash
gzip -dc docs/validation/erob-3axis-2026-11-18/run1-can.log.gz > build/bench-erob-3axis/run1-can.log
python3 scripts/analyze-bench-trace.py build/bench-erob-3axis/run1-can.log \
  --interface can0 --nodes 1,2,3 --period-us 10000 \
  --first-sync 24864 --last-sync 25911
```

`audit-shm-can.py` 保存了逐字段比较方法；运行时读取 `build/bench-erob-3axis/` 下的原始 CAN 日志和 `final-motion.csv`。


## 加速可视运动复验

用户反馈首次小幅轨迹不明显，并授权提高速度。新增显式 `visible` 模式，保留原 `move` 小幅验收模式。逐轴使能、每轴等待 700 ms 后，三轴各执行 45° 平滑余弦往返，周期 6 s，连续 3 次；总运动时间 18 s，规划峰值速度 23.56°/s。驱动保护和客户端 1024 count 跟随误差阈值保持原值。

[本次实测输出](visible-motion.txt)完整通过，[逐周期记录](visible-motion.csv)显示三个关节的位置变化范围均约 45.01°。速度反馈峰值分别约 24.39、24.47、24.31°/s；最大采样跟随误差为 404、400、404 count（约 0.28°），回到起点保持时误差为 -3、-1、-4 count。结束后三轴控制字 0、状态字 0x0240，正常失能。此轮使用 SHM 记录，未另做独立 CAN 抓包比对。测试二进制摘要见 [visible-binary-sha256.txt](visible-binary-sha256.txt)。

在已准备台架上复现（该命令会实际运动）：

```bash
./build/debug/bin/erob_csp_bench 2 visible build/visible-motion.csv
```


## 停机归档与长期运行限制

用户要求停止主站、创建提交时，检查发现主站及运动测试进程均已退出，因而未再发送停机信号。通过 SDO 再次确认三轴状态字均为 `0x0240`（失能）、控制字为 2（主站组停机命令）、0x603F 均为 0，见 [停止后状态读回](stopped-status.json)。can0 保持 UP，主站未重启。

[主站最终日志](master-final.log)显示：约 39.8 分钟、238877 个 SYNC 后，主站因 `invalid feedback window`（CAN-ID `0x282`，槽 1 / Node-ID 2）触发保护停机，累计 missed cycles 为 1。组停机收到三轴 0x0240 确认，RX 队列溢出和节点心跳故障计数均为 0。该异常的具体原因尚未定位；此前独立抓包已结束，不能用其判断这次异常是重复帧、过期帧还是其他窗口违规。短时运动测试通过不等于长期运行稳定，本次保留该限制，不宣称 M5 全部验收。

提交版本按本项目 AGENTS.md 从 `0.0.0-rc.6` 递增至 `0.0.0-rc.7`，唯一版本源为根目录 VERSION；保留历史证据中的原始版本和二进制摘要。

rc.7 提交前重新完成 Debug 配置、构建和显式台架工具构建，生成头文件确认为 `0.0.0-rc.7`；[CTest 8/8 通过](rc7-ctest.log)，[三轴配置离线检查通过](rc7-config-check.txt)。此次停机归档未重新启动主站或执行实物运动。
