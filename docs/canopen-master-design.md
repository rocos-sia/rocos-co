# ROCOS-CO · CANopen 主站设计评审稿

2026-11-12 · 拟建项目 · 首版面向 Linux x86_64、经典 CAN、单通道。文中的 YAML、命令、日志和构建目标均为拟议接口，可用来评审和编写验收用例。

建议把 `rocos_co_master` 做成与 rocos-ecm、rocos-igh 并列的独立进程。它向下管理一条 CANopen 总线，向上提供同一套 rocos-shm v2 过程映像、周期通知和单命令邮箱。App 继续负责关节绑定、单位换算、CiA 402 驱动状态机和运动控制。**已确定的首版测试工况为六个 ZeroErr eRob 关节、CiA 402 CSP、8 ms（125 Hz）。** 同一 rocos-app 通过 rocos-shm 访问 CO、ECM 和 IgH。App 改动范围仅限后端身份识别；首轮 rocos-co 独立监测主站周期和 CANopen 通信，运行健康判定不依赖 App 进程状态。额外 App 变更须单独确认。首版 App 按 SHM ID 连接，由 rocos-co 独立读取 co.yaml。六轴共同生效的边界为同一帧 SYNC，反馈到锁存以 8 ms 一拍为目标；停机时间先实测。

## 1. 与现有 ROCOS 的衔接

| 核对依据 | 对本项目的约束或取舍 |
| --- | --- |
| 飞书《ROCOS控制器软件架构设计说明》《机器人控制器数据流》 | 每个总线 ID 对应一个后端实例；进程在同一 Linux 主机通过 SHM 交换原始值。Viz、CFG 和机器人算法沿用各自职责。多通道可先部署多个独立主站；一个 App 跨总线做时钟同步另行设计。 |
| 飞书《ROCOS-ECM/IGH共享内存规范设计》及 rocos-shm 实施代码 | 直接链接公共库，沿用固定结构、容量、路径、所有权、有效位和超时语义。飞书早期 ECM 结构笔记不作为新布局依据。 |
| 飞书《ROCOS-APP配置文件规范设计》§5、§9.5；ECM、IgH 的配置与加载器 | 公用参数采用 `master.id/cycle/cpuidx` 等名称、单位及相对路径规则。ECM 示例用嵌套 `master`，IgH 同时有顶层 ENI 入口和旧的内联 PDO YAML。CO 选一种规范写法，沿用内联 `slaves/rx_pdos/tx_pdos/entries` 的设备视角。 |
| ECM 终端规范、IgH 的 startup_report、console_style 和运行汇总 | 保留分节、列名、单位、颜色退化及非实时输出位置。把 ENI 地址、DC/WKC 和 SDK 授权信息换成实际可观测的 Node-ID、COB-ID、心跳及接口状态。 |
| App 的 app_config.cpp、hardware.cpp、robot.cpp | 当前组合配置只接受 ecm、igh、mujoco；控制入口检查目标为 op 和周期一致。Hardware 等待槽 8，按设备 id、方向和名称缓存有宽度要求的 PDO，并已管理 CiA 402。**ABI 互通与 App 产品支持需分别验收。** |

一次部署中有三个不同的编号。以本文样例为例，`master.id=2` 选择 SHM 命名空间，`slaves[0].id=0` 选择 App 中设备的槽位，`node_id=1` 才是线上的 CANopen 地址。CAN 接口名 `can0` 与这三个编号独立。第二个后端即使使用不同的 SHM ID，也不能管理同一物理 CAN 网络；由部署配置独占通道及 SYNC/NMT 管理角色。

## 2. 进程和模块

```text
rocos-app (唯一可写客户端)       shm_web 等监控 (只读)
            ↕                           ↑
       rocos-shm v2 · 公共布局、过程映像、信号量、单邮箱
            ↕
  ShmAdapter ── ProcessImagePlan ── MasterRuntime
              配置编译与地址映射      启动、周期、故障、停机
                                      ↕
                CANopenNode + 主站本地 OD + CO_driver 移植层
                                      ↕
                                CanTransport
                            ↙         ↓         ↘
                       SocketCAN   内存测试替身   ZLG 适配器（后续）
```

| 模块与建议目录 | 所有权与接口 |
| --- | --- |
| `src/config/`、`config/` | yaml-cpp 读取 YAML，产生有类型、不可变的 MasterConfig。集中校验节点、报文、设备身份、映像容量与负载预算；输出含文件、行号和键路径的错误。 |
| `src/can/` | 传输对象独占句柄；只识别 CAN 帧、过滤器、收发状态、时间戳及链路诊断。SocketCAN 系统头仅在此后端使用。 |
| `src/canopen/`、`od/` | C 移植层、主站本地对象字典（OD）、本地 PDO 与远端节点对象的映射、NMT、SDO、SYNC、Heartbeat、EMCY。为每个节点保存身份、当前阶段、最近心跳和每帧到达代数。 |
| `src/master/`、`src/shm/` | 前者推进启动与周期、协调故障和关闭；后者调用公共 IPC API，维护稳定槽位、字节偏移和邮箱返回码。两者均不依赖 CAN 厂商 SDK。 |
| `src/main.cpp`、`src/report/` | 解析 CLI、组织生命周期、消费有限大小的诊断记录并打印。退出码区分正常退出、配置/依赖错误、启动失败和运行故障。 |
| `tests/`、`cmake/`、`3rdparty/` | 无硬件测试、依赖集成和锁定版本。设备字典、厂商库与项目自编适配代码分别管理。 |

### CANopenNode 集成与对象字典

选定并锁定经测试的 CANopenNode v4 系列提交。编译 C 栈，在 C++ 外层持有 `CO_t` 和设备上下文。启用 NMT master、Heartbeat consumer/producer、SDO client、SYNC producer、PDO 与 EMCY 接收；只编入本地 OD/协议初始化实际依赖的其余服务。主站自己占用一个唯一 Node-ID。与 CANopenLinux 对照 Linux 移植行为，最终进程只保留一个 CAN 接收分派者、一个定时调度者。

- **本地 OD 和从站字典分开。** 仓库保存本地主站 OD 源文件、生成的 OD.c/OD.h 和生成工具版本。OD 包含本地通信参数（如 0x1005/0x1006、0x1016/0x1017、0x1280）、PDO 通信/映射参数和按帧分配的厂商区镜像条目。远端 0x6041 等对象由节点自己的 EDS/设备资料解释。运行期 YAML 描述节点映射，首版无须解析全量 EDS 或实现 LSS 自动分配地址。
- **把 YAML 编译为本地 PDO 表。** 从站 TPDO 对应主站本地 RPDO，从站 RPDO 对应主站本地 TPDO，双方配对使用同一 COB-ID。本地条目索引只用于栈内搬运，SHM 描述符保留远端真实 index/sub_index。启动时配置镜像区、回调和过滤表，预检所需 PDO 对数、映射条目、Heartbeat consumer 和驱动缓冲数量是否落在构建 OD 的有效容量内。打印编译容量与实际使用量；ABI 允许 50 个槽位并不等于栈能管理 50 轴。
- **执行顺序。** 以锁定版本的 `CO_new`、`CO_CANinit`、`CO_CANopenInit`、`CO_CANopenInitPDO` 初始化，切入正常 CAN 模式；用 `CO_process` 推进后台协议处理，SDO client 用非阻塞上传/下载步骤独立推进。`CO_process_SYNC/RPDO/TPDO` 各有唯一调用点，按本地 OD 的同步类型及缓冲语义安排。启动和退出时处理栈的 reset 请求，重新配置后重建运行实例。对栈的错误、内存不足、非法 OD 或不可分配过滤表逐项报错退出。
- **状态归属。** NMT 管理节点通信状态，SYNC 约束报文节拍，CiA 402 控制字/状态字归 App。进入 NMT Operational 只开启过程数据交换。主站不主动把驱动切入 Operation Enabled，也不在恢复时重放上一轮目标。

上游参考：[CANopenNode](https://github.com/CANopenNode/CANopenNode)、[栈生命周期](https://canopennode.github.io/CANopenNode/CANopen_8h.html)、[PDO 模块](https://canopennode.github.io/CANopenNode/group__CO__PDO.html)、[CANopenLinux](https://github.com/CANopenNode/CANopenLinux)。

### CAN 传输接口与可替换性

| 操作 | 合同 |
| --- | --- |
| `open(options)`、`capabilities()` | 取得通道、校验经典 CAN、报告可读的位速率、时间戳来源、硬件过滤能力和诊断能力。根据实际能力选择内核或软件过滤。 |
| `setFilters(list)` | 根据栈的 RX 注册表订阅 NMT、SDO、PDO、SYNC、心跳、EMCY 等流量。底层粗过滤后仍由移植层按 CAN-ID、mask、帧类型匹配并调用注册回调。 |
| `receiveBatch(span, limit)`、`trySend(frame)` | 概念接口；C++17 实现用指针和容量即可。帧携带标准 ID、长度、最多 8 个字节及可选时间戳。操作有界、非阻塞；分别返回成功数量、暂时无数据/发送繁忙、队列溢出、断链或永久错误。发送成功只表示后端接收帧。 |
| `status()`、`close()` | 返回链路/错误状态及各自有效性；关闭须中止等待，等回调和正在使用的缓冲释放后再销毁资源。 |

CO_driver 移植层实现栈所需的 RX/TX buffer init、send、同步发送清理、CAN 模式切换和模块错误处理，保持栈的 `bufferFull`、同步窗口与错误语义。后端以编译期工厂选择；协议层不持有 fd 或 ZLG 设备句柄。首版 SocketCAN 用 `PF_CAN/SOCK_RAW/CAN_RAW` 绑定指定 ifindex，配置数据过滤及 `CAN_RAW_ERR_FILTER`，区分错误帧、RTR、扩展帧、本机回环和普通数据。接收队列溢出计数与 bus-off 单独报告；CAN FD 和扩展帧不进入首版 PDO。真实接口由部署侧预设 bitrate 和 UP；启动通过链路查询核对，参数不符即失败。vcan 没有物理位速率，使用配置值做离线预算并标记虚拟。

后续接 ZLG 时实现同一接口；若厂商只给阻塞批量接收，使用后端自有线程和预分配有界队列，记录丢帧和时间戳精度，关闭时唤醒并 join。动态库仅由该适配器装载，启动检查架构、符号与版本；缺库只影响选用它的实例。用同一组帧回放、背压、bus-off、退出期间回调测试验收两个后端。[CANopenNode driver 接口](https://canopennode.github.io/CANopenNode/group__CO__driver.html) · [Linux SocketCAN 文档](https://docs.kernel.org/networking/can.html)

## 3. 配置合同与可手算的映像

建议主站入口 `rocos_co_master --config <co.yaml>`，`--check-config` 与 `--config` 配合仅做静态检查、打印映像计划与总线预算；`--help` 独立使用。显式 CLI（首版建议 `--master-id`、`--period-us`、`--cpu`）覆盖 YAML，再回退到程序默认值，所有覆盖后重新校验。路径相对于声明它的文件，日志标注配置路径和值来源。co.yaml 由 rocos-co 独立读取。部署时核对两端的 SHM ID、周期、槽位和 PDO 名称/宽度；App 无须读取 CAN 接口、Node-ID、COB-ID 或 EDS。调试覆盖不回写文件。下列 YAML 为六轴配置的第一个节点节选，另外五个轴使用同一结构并换成各自的物理 Node-ID 与 COB-ID。

```yaml
version: 1
master:
  id: 2
  cycle: 8000                    # us；六轴 CSP 台架，125 Hz
  cpuidx: null                   # 继承亲和性
  duration: 0                    # ms；达到目标后计时，0 持续运行
  verbose: diagnostic
  state: op                      # CANopen 启动目标，preop 用于维护
  transport: socketcan
  interface: can0
  bitrate: 1000000               # bit/s，期望的实际总线速率
  node_id: 127                   # 主站本地 Node-ID
  heartbeat_ms: 100              # 主站生产心跳周期
  preop_timeout_ms: 10000
  op_timeout_ms: 10000
  command_timeout_ms: 1000
slaves:
  - id: 0
    node_id: 1                   # 示例物理地址，接线后逐台核对
    name: eRob-J1
    identity: {vendor_id: 0x5A65726F, product_code: 0x26483052}  # EDS 期望，启动逐台读回
    heartbeat_ms: 100            # 从站 0x1017:00 的期望生产周期
    heartbeat_timeout_ms: 300    # 主站监测从站，示例
    master_heartbeat_timeout_ms: 300  # 从站监测主站，示例；与主站监测从站分别设置
    pdo_policy: configure        # eRob 建议重映射并读回；已预置者可用 verify
    init_commands: []
    rx_pdos:                     # 从站接收 → SHM Output
      - index: 0x1600
        cob_id: 0x201
        transmission_type: 1
        entries:
          - {name: Control word, index: 0x6040, sub_index: 0, bit_length: 16}
          - {name: Target Position, index: 0x607A, sub_index: 0, bit_length: 32}
          - {name: Mode of operation, index: 0x6060, sub_index: 0, bit_length: 8}
    tx_pdos:                     # 从站发送 → SHM Input
      - index: 0x1A00
        cob_id: 0x181
        transmission_type: 1
        entries:
          - {name: Status word, index: 0x6041, sub_index: 0, bit_length: 16}
          - {name: Position actual value, index: 0x6064, sub_index: 0, bit_length: 32}
      - index: 0x1A01
        cob_id: 0x281
        transmission_type: 1
        entries:
          - {name: Velocity actual value, index: 0x606C, sub_index: 0, bit_length: 32}
          - {name: Mode display, index: 0x6061, sub_index: 0, bit_length: 8}
```

### 每轴每 8 ms 交换哪些数据

RPDO、TPDO 均从关节看：RPDO 是关节接收的命令帧，TPDO 是关节发出的反馈帧。下表是上方 YAML 建议的映射，n 代表关节 Node-ID，字节从 0 编号。对象索引和类型在配置阶段约定；运行时每帧数据区只携带各字段的值，多字节值按小端排列。

| 帧与数据区长度 | 帧内字节 | 对象 | 类型 | 这一拍表达什么 |
| --- | --- | --- | --- | --- |
| 主站 → 关节，RPDO1，0x200+n，7 字节 | 0–1 | 0x6040:00 控制字 | uint16 | App 请求的 CiA 402 驱动状态，如保持使能或请求快速停止。 |
| 同一 RPDO1 | 2–5 | 0x607A:00 目标位置 | int32 | 该轴本次要采用的位置设定值，使用驱动原始位置单位。 |
| 同一 RPDO1 | 6 | 0x6060:00 运行模式 | int8 | 请求的模式。CSP 的值为 8，持续运行时通常保持该值。 |
| 关节 → 主站，TPDO1，0x180+n，6 字节 | 0–1 | 0x6041:00 状态字 | uint16 | 驱动报告实际状态，如已使能或发生故障。 |
| 同一 TPDO1 | 2–5 | 0x6064:00 实际位置 | int32 | 该轴反馈的位置，用来与目标位置对照。 |
| 关节 → 主站，TPDO2，0x280+n，5 字节 | 0–3 | 0x606C:00 实际速度 | int32 | 该轴反馈的速度，供 App 做运动反馈检查。 |
| 同一 TPDO2 | 4 | 0x6061:00 模式显示 | int8 | 驱动报告当前生效的模式，CSP 时应读到 8。 |

每轴输入反馈合计 11 字节，超过经典 CAN 单帧 8 字节的数据区上限，因此分两帧。以 Node-ID 1 为例，每拍主站发 0x201，关节回 0x181、0x281。六轴共 6 帧命令、12 帧反馈，另有全总线共用的一帧 SYNC。7、6、5 都是数据区长度，线时预算另计帧头、校验、填充和帧间隔。

**SHM 映像。** 物理节点暂以 1–6 举例，SHM 槽 i=0–5 固定，现场地址可独立变更。状态、实际位置、实际速度和模式显示在 Input 的字节偏移依次为 11i + {0,2,6,10}；控制字、目标位置、模式在 Output 的偏移为 7i + {0,2,6}。六轴总计 Input 66、Output 42 bytes。App 的 hardware.yaml 使用对应 PDO 的精确名称；非自然对齐值按公共库 value API 读取，单位换算与机械零点由各轴型号配置负责。

**附件依据及适用范围。** 手册 V1.9 §3.4、§3.7.1、§5.5.1 给出单 SYNC 发生器、同步传输类型和 CSP 报文；EDS V1.5 的 DeviceInfo 仅将 1000 kbit/s 标为支持并声明 4 RxPDO / 4 TxPDO。据此选 1 Mbit/s 作测试配置，核对实际接口及所有关节。EDS 的 0x1018:01–03 列出 vendor、product 和 revision（0x00020111），未声明 :04 serial。EDS 默认 PDO1 各仅映射控制字/状态字，通信类型 255；TPDO2 默认禁用。按上述候选配置启用并读回全部所需 PDO，校验其余 PDO 不产生冲突或预算外报文。EDS 与手册用于拟定配置，在线身份与实际映射另行记录。

**8 ms 的来源。** 主站作为唯一 SYNC 生产者，本地 0x1006=8000 μs，关节作为消费者（0x1005 的 producer 位清零）。手册 §5.5.1 举的是 1000 μs 通信样例；§8.2.5 明确 0x1006 指生产周期。EDS 列出 0x60C2:01/:02，但手册 §8.2.118 将插补周期标为保留、暂不支持。8 ms 是指定的台架工况，需在实际固件上验收，不依赖写 0x60C2。使能前核验模式、实际位置及初始目标对齐，不直接执行手册报文示例。

| 字段组 | 首版拟议校验 |
| --- | --- |
| 主站公共项 | `id` 非负 int，默认 0；`cycle` 六轴测试显式 8000 μs、范围 1000–1000000 μs，仍须通过时隙预算和设备周期校验；`cpuidx` 可空或为在线且在 cpuset 内的 CPU；`duration` 为非负毫秒，默认 0。verbose 沿用 silent/errors/warnings/diagnostic/info/verbose/cyclic，默认 diagnostic，周期档也由报告线程聚合。 |
| CAN 参数 | `interface/bitrate/node_id` 显式指定，transport 默认 socketcan；主从地址均为 1–127，同网唯一；首版一个 SYNC 生产者、固定标准 SYNC ID、每周期同步 PDO（type 1）。`state` 默认 op，可取 preop 或 op，分别对应 NMT Pre-operational 和 Operational；拒绝 EtherCAT 的 safeop、eni、dc、SM 分配等字段。 |
| 超时与心跳 | 阶段超时为正毫秒，默认如样例；命令期限 1–60000 ms。心跳生产时间 1–65535 ms，消费超时在可编码范围内且大于生产周期加预计抖动。由主站统一配置、读回从站 0x1017 和 0x1016:01。后者的高 8 位保留，中间 8 位为主站 Node-ID，低 16 位为超时毫秒。台架样例 100/300 ms 满足手册建议的消费超时大于生产周期两倍；最终取值按响应要求和实测抖动确定。另验设备对主站心跳丢失、SYNC 丢失的响应。 |
| 节点、身份 | `slaves` 按连续且从 0 开始的 id 排列，1–50 项。期望 vendor/product 为部署时必填，revision/serial 可选；通过 0x1018 的相应子索引核实。0x100A 固件可选。缺少必需身份、与配置不符或字符串放不下均报告，禁用模糊匹配。 |
| PDO | 每个方向 ≤25 个 SHM 变量；每个经典 PDO 映射 ≤64 bit，宽度为正且字节对齐，合计与实收 DLC 一致；PDO 序号在设备/本地 OD 支持范围内。所有报文生产者 COB-ID 唯一、避开保留 ID 及管理流量；同一帧的生产端与消费端按设计配对。节点/方向内名称唯一，名称按 UTF-8 字节含 NUL 校验（PdVar 72、Slave 80）。允许单方向 0 个变量，如只读传感器。 |
| 初始化与变更 | `pdo_policy: verify` 默认逐项读回静态映射；`configure` 仅用于明确支持重映射的设备。`init_commands` 使用 IgH 的 name/index/sub_index/Type/Value，按类型做有符号、长度和溢出校验。只允许设备配置清单声明的对象，映射及主站管理对象由专用流程控制。配置版本不识别、重复键、未知键、无效 COB-ID 或预算不足在改写设备前失败。 |

## 4. 启动、周期与故障语义

1. **静态准备。** 加载并校验配置，生成映像与帧计划，打印配置摘要。运行模式先取得 SHM owner 锁并建立未 ready 的映射，再打开独占管理的 CAN 通道，初始化 OD 和协议栈。后续失败按逆序释放；抢占同 ID 失败时不改变对端节点状态。
2. **识别节点。** 监听心跳及 boot-up，能接管已经启动且发心跳的节点。只向清单内 Node-ID 发送定向 NMT 请求，通过后续心跳确认进入 Pre-operational。EDS 的 0x1017 默认为 0；冷启动若只有 boot-up 或已错过该帧，可定向请求 PREOP 并用有期限的 SDO 读 0x1018 核对身份，再配置 0x1017。收到首条有效心跳后确认 NMT 状态，再开始 PDO 改写，避免等待尚未启用的心跳。辨识失败、重复地址迹象、外部 SYNC 冲突或 bus-off 均阻止进入业务。相同地址设备无法仅靠被动扫描可靠区分，现场地址规划也要核对。
3. **准备映射。** 按节点顺序执行已批准的初始化命令。verify 读取通信参数及映射条目；configure 按对应通信对象 0x1400/0x1800 系列禁用 PDO、清零映射数量、写入 0x1600/0x1A00 系列条目、恢复数量、设置传输类型/COB-ID 并启用。逐项读回 COB-ID、有效位、类型、数量、条目顺序与位宽。固定映射也须有设备可验证的映射资料和读回能力，未验证型号留待适配。中途失败保持节点在维护状态，报告最后确认步骤，重新读取实际配置再决定恢复；写结果不明时不自动重放。所有操作共享 PREOP 阶段截止时间。
4. **达到启动目标。** preop 维护模式发布已验证的描述与邮箱，禁止过程输出，禁止控制 App 启动。op 模式配置主站本地 PDO 与心跳消费者、进入本地 Operational，定向启动各节点；发送 SYNC 以获取输入，业务输出仍关闭。等全部必需节点确认 Operational、身份/映射一致、无阻断故障、连续若干完整观测周期（初值建议 3 个）后，先填好真实 Input，再 `publishReady()` 并发首次通知。收到过期或错误 DLC 的帧不能满足门槛。OP 阶段超时则记录仍缺少的节点/帧并退出。
5. **运行与退出。** master 周期以 `CLOCK_MONOTONIC` 绝对时间驱动，错过周期跳至下一截止点、计数，不密集补发 SYNC。正常退出关闭业务输出、在链路可用时请求节点进入约定停机状态并限时确认，关闭邮箱、周期/接收、CAN 句柄后再释放 IPC；异常时先阻止新的输出。现场停机动作需和具体驱动器失联策略联合验证。

**协议栈所有权。** 建议由一个有界事件循环独占 CANopenNode 与通道，按定时器和接收事件推进协议，合并同周期的多个帧。管理/邮箱线程处理配置、等待、上层事务；仅向栈线程提交一份预分配的进行中 SDO 描述，由栈线程在预算内推进一步，完成后回传私有结果。初始化可使用相同事件泵等待阶段条件。周期中不调用阻塞式 SDO、不申请堆、不解析 YAML、不格式化日志。记录每次循环允许的最大帧数和管理工作耗时；优先处理同步窗口、紧急错误及即将到期的事务。报告线程独立输出累计计数。

### 同一个 SYNC、同一批目标、反馈到锁存

**六轴共同生效。** 每 8 ms 由 rocos-co 发一个 COB-ID 0x080 的 SYNC。手册 §3.7.1、§8.2.19 规定同步 RPDO 先缓存在驱动，收到下一帧 SYNC 时应用。六个目标帧在 CAN 上依次发送，只要六轴在同一 SYNC 前均已收到对应目标，就能在共同的同步边界采用目标。此处的“锁存”指采用新设定值；各驱动内部控制环相位、机械响应和实际到位时间仍须分别观测。CAN 的链路 ACK 不逐轴确认指定驱动已采用该目标。

| 例：反馈属于 0 ms 的 SYNC[k] | 台架记录 |
| --- | --- |
| 0 ms 附近 | 六轴产生该拍反馈，12 个 TPDO 依次到线。主站保存帧的窗口编号和收齐时刻，再发布完整 Input、更新 Unix μs 的 timestamp、通知槽 8；故障期间仅主站活性可继续更新。 |
| 两个 SYNC 之间 | App 使用反馈计算六轴目标。主站在预定相位把 Output 拷入私有发送区，准备六帧，并留足最迟一帧上总线的时间。记录 App 可用计算时间、采样相位和线上最后一帧的余量。 |
| 8 ms 或 16 ms 附近 | 若这批反馈对应的六帧赶上 SYNC[k+1]，理论反馈到锁存约一拍；若采用固定流水，在 SYNC[k+2] 采用，则约两拍。两种情况都可以每 8 ms 更新一次目标。同批目标的年龄不同，会改变跟随滞后。偶发错拍须单列，不能算入固定流水。 |

主站若使用 CANopenNode 的**同步本地 RPDO**接收远端 TPDO，数据在下一次 SYNC 处理时才转入本地 OD；主站本地同步 TPDO 的发送也受调用相位约束。首版目标为 SYNC[k] 对应的反馈所算出的六轴目标在同一 SYNC[k+1] 生效（标称 8 ms）。台架记录每次测试的周期数、最坏反馈到锁存延迟和漏拍数。以 k+1 为预期边界，标记未赶上的周期及受影响轴，区分固定的额外流水延迟和偶发漏拍。验收同时采集线上 SYNC、反馈和目标帧，配合可用的驱动内部目标/采样记录。若只能抓 CAN，只能确认线上相位；内部锁存偏差须标“未测”。[CANopenNode PDO 收发语义](https://canopennode.github.io/CANopenNode/group__CO__PDO.html)

**“严格同代”另有一层要求。** 假设 App 正写第 101 组目标，前三轴已更新、后三轴还保留第 100 组，主站此刻复制 Output，仍可把六帧赶在同一 SYNC 前送到。驱动采用时刻一致，目标却来自两次计算。v2 的周期信号由主站发给 App，没有 App 对整组命令写完的反向提交标记。主站一次复制只能稳定后续发送的字节。首版验收六轴目标同一 SYNC 生效，暂不要求 App 证明六轴命令严格同代。线上部分帧已经发出时也无法撤回；不能承诺掉帧情况下六台驱动具备原子提交或回滚能力。

### 故障由哪一层发现、哪一层停机

首轮台架暂不设停机时间上限。以注入故障的时刻为共同起点，分别测量 **T1 检测/锁故障、T2 驱动进入约定停止状态、T3 六轴实际速度降至约定阈值**。T3 包含负载和减速过程；如另有抱闸或切断力矩要求，单列终点。8 ms 是采样节拍，不能据此推得 T2、T3 的上限。

| 场景 | rocos-co 的职责 | 其他责任与验收 |
| --- | --- | --- |
| 部分 TPDO 缺失、节点重启/离开 OP、阻断性 EMCY、发送积压 | 按周期窗口检查 12 个必需 TPDO、DLC、心跳及错误状态；建议首个不完整窗口锁定六轴组故障。停止发布旧反馈与正常目标，链路可用时保留专用的组停机输出及驱动执行该输出所需的 SYNC，限时观察状态字和速度。清理过期发送并记录在途帧和未确认节点。 | 状态字与速度分别作为 T2、T3 证据；若失去反馈，记录未确认，不推断已停止。 |
| 主站退出、CAN 断线或 bus-off | 链路可用时发受控停止；链路丢失时主站若仍运行，可记录故障并禁止恢复后自动使能；主站进程崩溃时由驱动侧独立响应。 | 驱动本地须有经核验的失联反应。双向心跳：主站监测每轴 0x1017；各轴 0x1016:01 监测主站 Node-ID 和超时。EDS 两端对应设备对象默认值均为 0；手册 §8.2.15 指出首帧后才开始监测，§7.2.23 把使能时的 0x8130 心跳报警注明适用 X3071220X 及以上固件。启动读回双向设置、记录实际固件；独立注入“只失 SYNC、心跳仍正常”。手册 §8.2.61 有 0x605A，所附 EDS 未列此项，0x6085 的减速行为及通信报警后实际动作均须台架核对。 |

恢复前重新核验六轴身份、映射、状态及目标与当前位置对齐，显式重新使能。台架报告列出检测方的 T1、各轴的 T2/T3，并以最后一个轴达到停稳判据的时刻记整组 T3；示例心跳 100/300 ms 仅用于说明双向配置，300 ms 为示例检测超时，不能作整机停稳承诺。

### 总线预算直接决定周期和轴数

静态检查对每个周期必发的 SYNC、TPDO、RPDO 按 DLC 累加最坏填充上界；对心跳、限额内的 SDO 和诊断流量预留时隙。标准 11-bit 数据帧含帧间隔的保守估算为 `47 + 8·DLC + floor((34 + 8·DLC - 1)/4)` bit。它覆盖正常帧位填充；仲裁等待、错误重发、主机调度和 USB 批传还需在时延分析和实测中计入。

六轴、1 Mbit/s、8 ms，每拍 6 个 7 字节 RPDO、6 个 6 字节 TPDO、6 个 5 字节 TPDO 和 1 个空 SYNC，合计 19 帧。按上式为 6 × (125 + 115 + 105) + 55 = **2125 bit，即 2.125 ms、26.56%**。扣除这些周期帧后有 5.875 ms 名义余量，由心跳/EMCY、其他报文、软件调度和相位裕量共享。驱动若启用额外 PDO、数据宽度变化或有竞争节点，须重算。验收看最迟目标帧到下一 SYNC 的余量及最慢轴锁存延迟，平均利用率仅作参考。

## 5. rocos-shm v2 的逐字段适配

构建时包含 `<rocos_shm/shared_memory_config.hpp>` 并链接 `rocos::shm`。Linux x86_64 LP64、小端、自然对齐；使用库内 static_assert 和连接检查，不复制结构体。80-byte ShmHeader（48 字节 magic 区，前 8 字节为 ROCOSHM2）、220088-byte EcatBus、220168-byte SharedMemoryV2；Slave 4396、PdVar 84、AsyncCommandBox 128 bytes。包版本 2.0.0 与布局版本 2 分开显示。

| 公共字段或对象 | CO 拟议行为 |
| --- | --- |
| IPC 名称、所有权 | 直接使用库创建的 `/ecm_v2_{id}`、`/pd_input_v2_{id}`、`/pd_output_v2_{id}`、`/sync_v2_{id}_{0..9}`。每 ID 一个 owner、一个可写 App，其他工具 `connect(true)`；禁止只读监控消耗通知。任一端崩溃后关闭旧连接并协调重启两端，无热接管。 |
| 生命周期 | `createSharedMemory()` → `createPdDataMemoryProvider(inBytes,outBytes)` → 填静态元数据及周期初值 → 按启动目标 `publishReady()` → 正常业务周期 `notifyClients()`。检查每个返回值。ready 表示布局已发布，不充当运行健康位；故障后不反复切换 ready。 |
| 槽位与描述符 | `slave_num` 为配置数；`slaves[i].id=i`。每个 PdVar 保留变量名、远端对象索引、子索引、所在方向的绝对字节 offset 和字节 size；以 `setPdoBits` 验界。映像长度精确计算，可有零字节方向。node_id、COB-ID、帧内 offset 存主站私有表并列入启动报告。 |
| 时间与版本 | `dt` 为设定周期 μs，周期实测 min/max/avg/current 单位 μs；`timestamp` 为 Unix μs；`heartbeat_mono_ns` 为主站活性单调时钟 ns，独立于从站 CAN 心跳。`ecm_version` 写本产品版本；无运行授权机制时 `is_authorized=true`，终端注明 N/A。 |
| 后端识别 | 已确定在现有 backend_type 内增加 co。按当前公共规范 0=unknown、1=EC-Master、2=IgH，建议在 rocos-shm 统一登记 **3=co**（拟分配，当前代码尚未实现）。沿用现有字段宽度、偏移和 ABI v2，不占保留字节。App 识别/展示该代号，运行时使用一致的 PDO、邮箱及能力位。 |
| 状态与诊断 | `current_state=0`（未知 AL），`state_control_supported=0`，忽略 request_state。显式覆盖库中的默认 INIT；不在 AL 字段写入 NMT 值。`al_state/al_status_code` 的 runtime_valid 位清零。WKC 保持 unknown、status_valid 对应位清零；DC 为 disabled、metric 无效。SYNC 抖动、NMT、EMCY 和 CAN 错误使用独立日志/统计展示。 |
| 在线、链路与身份 | 每个配置节点以心跳新鲜度更新 online 及有效位，connected_slave_num 统计在线配置节点；CAN 控制器可可靠报告链路状态时才置 link 有效位，接口 UP 不能证明远端在线。`info_valid` 逐项标识实际获取的 0x1018 字段和固件，`identity_source=1` 表示设备读回；未取得的可选字段清位。静态身份及映射在 ready 后保持不变。 |
| 并发 | 主站写 Input、App 写 Output；实时标量双方用库的 atomicLoad/atomicStore。通知只提示有新周期，单个进程的暂存区能保证其发布前数据齐全；v2 没有整幅快照、跨 PDO 原子写或客户端消费确认。 |

**邮箱。** 单独的非实时工作线程调用 `processCommand(handler,maxMs,preflight)`。首次实现建议支持 SDO_READ、SDO_WRITE、RESET_STATS 的相应位；SDO 限经确认的 preop 维护模式，op 模式下返回 STATE_MISMATCH。受支持位描述实现能力，当前可执行性另由状态约束。RESET_FAULT 仍为 UNSUPPORTED。维护模式下若需调试客户端写邮箱，它占用唯一可写客户端位置。

- 请求的 target_slave 是 SHM 槽位；映射到 node_id 后，再核实在线、身份、当前 NMT 状态及对象访问清单。公共库校验 cmd_id、instance_id、绝对单调截止时间和 1–64 字节请求范围。SDO 使用 expedited 或 segmented 传输承载原始字节，区分 64 字节邮箱载荷与 8 字节 CAN 帧；超容量读报 BUFFER_TOO_SMALL。
- 运行邮箱禁止改 PDO 映射、通信 COB-ID、心跳/SYNC、状态切换以及影响运行布局的对象；设备参数写采用允许清单。切换与已受理命令串行，硬件故障停机优先。收到远端 abort 才设置 sdo_abort_code 有效位，驱动错误单独标记 backend_error；未发出且过期为 DEADLINE_EXCEEDED，可能已写入而无法确认则 OUTCOME_UNKNOWN。
- 握手沿用 IDLE → REQUESTED → PROCESSING → SUCCESS/FAILED/TIMEOUT_UNKNOWN → IDLE。调用方本地等待超时后继续 poll 原事务，后端保证不再访问私有缓冲才结束处理；不自动重发写。RESET_STATS 使用 target_slave=-1、长度 0，在主站周期/维护节拍边界完成清零并确认后才回成功。

可对照 [公共 ABI 规范](../../rocos-shm/docs/abi-v2.md)、[布局声明](../../rocos-shm/include/rocos_shm/abi.hpp)、[公共库生命周期](../../rocos-shm/README.md)、[App PDO 访问实现](../../rocos-app/src/pdo_entry.hpp)。

## 6. 终端输出

配置校验通过后、打开 CAN 前先打印有效参数、后端版本/提交、配置绝对路径与覆盖来源。随后按阶段打印已验证的事实；启动摘要即便在 silent 档也可见。正常诊断走 stdout，配置/启动失败走 stderr，失败时退出非零。键名左对齐、最小宽 25，分隔为 ` : `，缩进两空格，分节线用 ASCII；TTY 下标题青色、成功绿色、告警黄色、失败红色，NO_COLOR、TERM=dumb 或重定向时纯文本。完整版本、路径及名称不截断。

```text
[Startup] ROCOS CANopen Master ==================================
  backend                   : co (backend_type=3, proposed) / CANopenNode <revision> / socketcan
[Config] Loaded settings (defaults included) --------------------
  master.id                 : 2 (IPC namespace)
  master.cycle              : 8000 us
[SHM] ABI=2 header=80 bytes bus=220088 bytes slaves=50 vars=25/25
      bus=/ecm_v2_2  sync=/sync_v2_2_{0..9}
[License] N/A - no runtime license check
[CAN] interface=can0  bitrate=1000000 bit/s  local-node=0x7f
[SHM] Process image: Input=66 bytes  Output=42 bytes
[PDO] Slave[0] eRob-J1 | node=0x01 | identity=online
  Direction    COB-ID  PDO     Index:Sub   Bits  Offset(bit)  Byte.Bit  Name
  Input/TxPDO  0x181   0x1a00  0x6041:00     16            0       0.0  Status word
  Input/TxPDO  0x181   0x1a00  0x6064:00     32           16       2.0  Position actual value
  Input/TxPDO  0x281   0x1a01  0x606c:00     32           48       6.0  Velocity actual value
  Input/TxPDO  0x281   0x1a01  0x6061:00      8           80      10.0  Mode display
  Output/RxPDO 0x201   0x1600  0x6040:00     16            0       0.0  Control word
  Output/RxPDO 0x201   0x1600  0x607a:00     32           16       2.0  Target Position
  Output/RxPDO 0x201   0x1600  0x6060:00      8           48       6.0  Mode of operation
  ... (5 more nodes)
[NMT] Operational: 6/6; required TPDOs received: 12/12
[Ready] master=2  startup target=op  process data available
```

实际输出另列产品版本、input/output 对象名、预期与实测身份、每个节点的全部映射和无变量方向的 `(none)`。配置错误格式沿用 `Configuration error (<path>): <key / reason>`。运行故障汇总带阶段、槽位和 Node-ID、CAN-ID/对象、期望/实际值、最后一次观测时间及原始 abort 或后端错误。故障发生与恢复立即排入诊断队列，相同告警限频并累加次数。周期汇总展示 SYNC 间隔、最迟 TPDO、截止时间错过、队列溢出和节点掉线；缺少真实硬件时间戳时注明采用的主机时钟。[ECM 终端规范](../../rocos-ecm/Doc/terminal_output.md) · [IgH 启动输出](../../rocos-igh/src/startup_report.cpp)

## 7. CMake、工程配置与交付

- 建议 CMake ≥3.20，project 启用 C/CXX，CANopenNode 的 C 标准按锁定版本配置，应用 C++17。目标划分为上游静态库 `canopennode`、包含配置/栈移植/运行时/SHM 适配的 `rocos_co_core` 和可执行文件 `rocos_co_master`。同一组 OD 配置宏和 `CO_driver_target.h` 对相关 C/C++ 翻译单元一致生效，避免结构体大小不一致。Ubuntu 22.04 通过 apt 安装 `libyaml-cpp-dev`，CMake 用 `find_package(yaml-cpp CONFIG REQUIRED)` 导入系统库；其余依赖包括 Threads、Linux CAN UAPI 和 `rocos::shm`；公共库子项目关闭不需要的 Web 工具与测试。
- CANopenNode、rocos-shm 固定子模块提交，默认离线构建，不在 configure 时跟踪远端分支。允许显式提供本地源码目录做联调；配置摘要打印实际依赖来源/提交，发布记录固定组合。保留 CANopenNode Apache-2.0 和 rocos-shm GPL-3.0-or-later 的许可证与通知，后续分发厂商库需核对其单独条款。
- 提供 debug/release 配置、构建和测试 presets，`BUILD_TESTING` 默认覆盖无硬件用例；vcan / 真机另加标签并显式启用。拟议命令为 `cmake --preset debug`、`cmake --build --preset debug --parallel 2`、`ctest --preset debug --output-on-failure`。安装使用 GNUInstallDirs，建议前缀 `/opt/rocos/co`，产物包括 `bin/rocos_co_master`、示例 YAML、设计和第三方许可。二进制只凭显式配置启动。
- **App 后端透明。** 已核对现有 `loadLegacy` 入口支持 robot.ecat_id、hardware_config 和 URDF，Hardware 从 SHM 读取 dt 并等待槽 8。首版台架使用这条现成的连接路径和六轴模型配置，由 rocos-co 单独读取 co.yaml。组合工程 schema_version: 1 目前限定 ecm/igh/mujoco 且有专属配置导出逻辑，不能只添 backend_type 就宣称支持 co 的组合工程。若要求这种入口，另行提交“通用 SHM 连接参数与后端私有配置分离”方案供确认。任何额外 App 变更须另行确认。CFG 工程导出留作后续独立需求。[现成连接入口](../../rocos-app/src/app_config.hpp) · [组合配置及导出](../../rocos-app/src/app_config.cpp)

## 8. 实施顺序与验收证据

| 阶段 | 可交付成果 | 进入下一阶段的证据 |
| --- | --- | --- |
| M1：构建与配置 | CMake 离线构建、固定依赖、OD、配置检查器和映像编译器 | 干净构建两种 preset；六轴输入/输出分别为 66/42 bytes，逐轴验证偏移与 19 帧预算。覆盖重复 ID/名称、COB-ID 冲突、9 字节帧、位变量、无输出设备、50/51 槽、25/26 变量、负周期、超载及覆盖后的重验。 |
| M2：协议与后端 | CO_driver、SocketCAN、FakeTransport、节点启动和安全退出 | 用独立的 CANopen 从站进程在预先建立的 vcan 上跑 boot-up/心跳、身份、SDO 分段、固定/可变 PDO、SYNC、NMT。核对逐帧方向/字节序，注入缺帧、错误 DLC、心跳超时、abort、队列满、部分映射失败，观察阶段、锁故障和退出。 |
| M3：SHM 互操作 | 映像、周期通知、命令邮箱、CLI 和启动报告 | 分进程用 App 所锁公共头编译读写客户端，加只读监控。验证唯一 owner、旧实例/坏长度拒绝、槽 8、互逆方向、非自然对齐读写、64/65 字节、迟到响应、无写重放、故障停通知及协调重启。快照测试检查顺序/单位/非 TTY 无 ANSI，并检查周期路径无日志和堆分配。 |
| M4：App 联调 | 三种后端共用 Hardware/PdoEntry 的同一条数据路径；CO 仅新增身份识别 | 先用无硬件写端和现有连接入口验证 6 个槽位、dt=8000、必需 PDO 精确名称与宽度、反馈为新样本、初始目标与实际位置一致。首拍丢失由台架启动超时报告；组合配置改造另审。 |
| M5：六轴物理总线 | ZeroErr eRob CSP、8 ms、逐轴型号/固件/PDO 读回和相位记录 | 先失能验证地址、初始值及双向心跳。记录 12 帧输入收齐、6 帧输出上总线、SYNC、驱动可观测锁存和各轴间偏差；以一拍为目标，记录六轴共同锁存边界、最坏延迟和漏拍。分别注入缺单帧、只失 SYNC、主站退出、bus-off；测量 T1/T2/T3，标注未能观测的项，不设停机时间上限。App 进程状态监测及其联停不列入首轮验收。 |
| M6：第二后端 | 需要 ZLG 时增加专有实现 | 在相同映像、协议与终端测试下替换 transport；分别记录批处理延迟和停机响应差异。CANopenNode、App 与公共 ABI 均无厂商类型依赖。 |

### 首版评审决定

1. **集成。** 六个 eRob 关节运行 CSP。App 按 SHM ID 连接，rocos-co 独立读取 co.yaml。App 改动仅限 co 后端身份识别。
2. **同步。** 六轴目标在同一 SYNC 生效，以反馈到锁存 8 ms 一拍为目标。记录最坏延迟和漏拍；暂不要求 App 证明六轴命令严格同代。
3. **停机。** 台架分别测量故障发现、驱动进入停止状态和六轴实际停稳的时间，首轮暂不设上限。记录每次注入的故障、检测方及所用状态和速度证据。

4. **主站健康监测。** 首轮 rocos-co 负责自身周期调度、CAN 接口收发、SYNC/PDO 时序及关节通信故障处置。App 意外退出或明确暂停本身不触发首轮主站组停机。首轮不实现 App 状态检测或相应的停机通知接口。

### 后续可选：由启动管理器通知组停机

仅当后续需求明确要求 App 与主站联停时，由启动管理器检测其启动的 App 实例是否意外退出或明确暂停，并通知对应的 rocos-co 实例执行六轴组停机。启动管理器负责判定 App 生命周期事件，rocos-co 负责执行六轴停机并记录可观测的驱动状态和速度。通知方式与联调验收在该需求启用时另行设计。

## 核对记录

- 台架资料：[eRob CANopen and EtherCAT 用户手册 V1.9](</home/think/Downloads/eRob CANopen and EtherCAT用户手册v1.9.pdf>)（印刷页 9–10、17、54–60、137、160–163、189、208，PDF 页序比印刷页大 5）；[ZeroErr Driver V1.5 EDS](</home/think/Downloads/ZeroErr Driver_V1.5.eds>)（DeviceInfo、0x1016–0x1018、0x1400/0x1600、0x1800–01/0x1A00–01、CiA 402 对象）。链接指向本次评审提供的本机原件。

飞书目录中的《ROCOS-ECM/IGH共享内存规范设计》是 v2 实施版，《ROCOS-APP配置文件规范设计》标注为目标设计。上文按当前源码标出配置入口、能力位和消费者限制，后续联合变更应同步维护公共规范。

- 飞书：[控制器项目目录](https://rocos-sia.feishu.cn/wiki/CjkywCBLZihnKnkFvFVcAPxdnmh) · [控制器架构](https://rocos-sia.feishu.cn/wiki/R4Hkw0KmUi6XemkEDkecYgdmnQf) · [控制器数据流](https://rocos-sia.feishu.cn/wiki/TCLsw6KAlihClfkTUxtcyexKnsc) · [共享内存规范](https://rocos-sia.feishu.cn/wiki/DQXzwAy0viTXCJkRAEpcrpsXnsg) · [配置规范 §9.5](https://rocos-sia.feishu.cn/wiki/N1QKwpKtEicos3kpt55csqSrnrg)。
- 本次工作区 HEAD：rocos-app `ce59e5536ed6`、rocos-ecm `63f62b8d265b`、rocos-igh `5cafe853f5bc`、rocos-shm `14618b7d7502`。App、ECM、IgH 锁定的 rocos-shm 提交分别为 `14618b7d7502`、`efbdb1afc153`、`87c436c49777`。实施时用同一发布组合跑交叉进程测试。
- 配置与消费端入口：[ECM 启动 YAML](../../rocos-ecm/config/config.yaml) · [IgH 启动 YAML](../../rocos-igh/config/igh.yaml) · [IgH 内联 PDO](../../rocos-igh/config/talon_pdo.yaml) · [App 配置加载](../../rocos-app/src/app_config.cpp) · [Hardware 周期与 PDO 缓存](../../rocos-app/src/hardware.cpp) · [机器人启动检查](../../rocos-app/src/robot.cpp)。
