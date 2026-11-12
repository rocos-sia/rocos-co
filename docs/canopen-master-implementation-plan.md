# ROCOS-CO 首版实施计划（待确认）

依据 [CANopen 主站设计方案](canopen-master-design.md)，评审对象是从空项目做到**六个 ZeroErr eRob 关节、CSP、8 ms（125 Hz）**台架验收的工作。Ubuntu 22.04 系统依赖安装脚本已提供；主站构建、代码和验收任务待计划确认后实施。

首版由 rocos-co 独立读取 `co.yaml`，使用 CANopenNode 和 SocketCAN。App 按 SHM ID 连接，沿用 rocos-shm v2；App 的代码改动限于 co 后端身份识别。主站负责自己的运行状态、总线及关节通信。App 挂起监督、启动管理器联停、证明六轴目标来自 App 同一次计算、ZLG 后端实现均留在首版范围之外。

## 阶段与验收环境

| 阶段 | 前置条件 | 阶段交付物 | 验收环境 |
| --- | --- | --- | --- |
| M1 工程与配置 | 计划确认 | 可复现构建、六轴配置、映像与报文预算检查器 | 纯离线 |
| M2 CANopen 主站 | M1 的构建、对象字典和配置合同 | 传输适配、节点启动、同步周期、故障处置；六节点模拟从站程序 | 纯离线 + vcan |
| M3 SHM 与运维输出 | M1 的映像；联合运行需要 M2 | v2 适配器、命令邮箱、主站可执行文件、终端报告 | 纯离线跨进程 + vcan |
| M4 现有 App 联调 | M2、M3；六轴模型与硬件绑定配置 | 同一 SHM 数据路径上的 App 联调记录、候选构建及部署配置 | 无物理硬件，vcan + 真实 App |
| M5 六轴台架 | M1–M4 通过；六台关节、适配器、模型和采集条件到位 | 实物配置读回、同步时序和分故障停机报告 | 必须使用六轴 8 ms 物理台架 |

纯离线测试使用内存传输替身、可控时钟或本机 POSIX SHM，无需 CAN 接口。vcan 测试使用预先建立的 Linux 虚拟接口、真实 SocketCAN 代码和独立从站进程。vcan 可验证报文和程序行为；真实总线仲裁、适配器排队、驱动锁存及机械停稳由 M5 提供证据。M3 的布局和邮箱测试可与 M2 并行开发。

## 拟定的仓库布局与 CMake 管理方式（供确认）

### 文件放在哪里

下图是实施时逐步建立的目录。根目录负责项目入口；模块的实现和私有头文件放在一起。首版对外提供主站可执行文件，跨进程接口由 rocos-shm 提供，因此不用另建一套公开的 CO 头文件。

```text
rocos-co/
├── CMakeLists.txt                  # 工程版本、平台、选项、子目录与安装
├── CMakePresets.json               # 团队共用的 debug/release 配置、构建、测试预设
├── VERSION                         # rocos-co 自身版本，含预发布后缀
├── README.md                       # 获取依赖、构建、离线验证和启动入口
├── .gitignore                      # build/、本机 CMakeUserPresets.json 等
├── .gitmodules                     # 子模块来源和路径，具体提交由父仓库锁定
├── 3rdparty/
│   ├── CANopenNode/                # 上游 CANopen C 协议栈
│   └── rocos-shm/                  # ROCOS 共用 SHM ABI 与访问库
├── scripts/
│   └── install-deps-ubuntu-22.04.sh # 使用 apt 安装系统依赖和 CAN 工具
├── cmake/
│   ├── Dependencies.cmake          # 检查依赖、接入子项目和系统库
│   └── CANopenNode.cmake           # 明列栈源文件、驱动头路径和 OD 编译配置
├── src/
│   ├── CMakeLists.txt              # 定义主站核心库与可执行文件
│   ├── main.cpp                    # CLI、退出码与进程生命周期
│   ├── config/                     # co.yaml 解析、校验、映像和报文预算编译
│   ├── can/                        # 帧/传输接口、SocketCAN 实现
│   ├── canopen/                    # 节点配置、SDO/NMT、PDO 与 OD 绑定
│   │   └── port/                   # CO_driver_target.h、驱动移植与 C 回调边界
│   ├── master/                     # 启动、周期调度、组故障与停机
│   ├── shm/                        # 公共 ABI 适配、映像发布、邮箱处理
│   └── report/                     # 非实时终端输出与诊断汇总
├── od/
│   ├── master.xdd                  # rocos-co 主站自己的 OD 源文件
│   └── generated/                  # 对应的 OD.c、OD.h，与源文件一起提交
├── config/
│   └── erob_6axis/co.yaml          # 首版六轴 CSP、8 ms 的主站配置示例
├── tests/
│   ├── CMakeLists.txt              # 测试目标、CTest 标签与注册条件
│   ├── unit/                       # 配置、帧编解码、时序及状态机
│   ├── ipc/                        # 多进程 SHM、邮箱和只读监控测试
│   ├── vcan/                       # 独立模拟从站与 SocketCAN 集成测试
│   ├── support/                    # FakeTransport、可控时钟、通用夹具
│   └── fixtures/                   # 最小 YAML、错误配置和期望输出
├── docs/                           # 设计、实施计划、台架规程和验收报告
└── build/<preset>/                 # 忽略入库；bin/、lib/、生成头及 CTest 结果
```

- **主站配置与测试数据分开维护。** 可部署的六轴示例只维护在 `config/erob_6axis/co.yaml`；完整示例的回归测试直接读取该文件，反例放在 `tests/fixtures/`。现场校准的配置由部署侧单独保存，升级安装只交付示例。App 的 URDF、关节换算和 hardware.yaml 沿用 App 工程管理。
- **主站 OD 与关节 EDS 分开管理。** `od/` 保存主站 OD，并记录生成工具版本及重建步骤；日常构建直接用已提交的生成文件。ZeroErr EDS 和手册是从站参考资料，在文档中记录版本与来源；构建不依赖开发机的 Downloads 路径。若后续交付允许随包分发的 EDS，可随对应设备配置放置。
- 源码内的 SocketCAN 实现由 `src/can/` 维护；测试替身和 vcan 从站只进入测试目标。ZLG 的源码目录、厂商 SDK 和构建开关待有接入任务时再建。

### 依赖纳管与升级

Ubuntu 22.04 上，在仓库根目录运行下列命令。脚本刷新 apt 索引，安装 C/C++ 工具链、CMake、Ninja、Git、yaml-cpp 开发包、Linux CAN 头文件及 SocketCAN 调试工具；普通用户由脚本调用 sudo，root 可直接运行。源码子模块按下文单独初始化。[查看安装脚本](../scripts/install-deps-ubuntu-22.04.sh)

```sh
./scripts/install-deps-ubuntu-22.04.sh
```

| 依赖 | 首版建议 | CMake 接入及更新时重点检查 |
| --- | --- | --- |
| CANopenNode | 在 `3rdparty/CANopenNode` 使用 Git submodule，锁定经验证的 v4 系列提交。 | 用项目自己的 `CANopenNode.cmake` 从清单构建静态栈库，移植代码留在本仓库。升级时核对源文件、配置宏、生成 OD 和 SYNC/PDO 语义，复跑 M2。 [上游仓库](https://github.com/CANopenNode/CANopenNode) |
| rocos-shm | 在 `3rdparty/rocos-shm` 使用 Git submodule，与 rocos-ecm、rocos-igh 的组织方式一致。 | 通过 `add_subdirectory` 使用上游 `rocos::shm`；主站构建关闭上游 Web 工具，上游独立测试在兼容性验证时单独启用。升级须对齐 App 端的 ABI、后端代号和能力位，复跑 M3/M4。 [公共库](https://github.com/rocos-sia/rocos-shm) |
| yaml-cpp | 从 Ubuntu apt 安装 `libyaml-cpp-dev`。 | CMake 用 `find_package(yaml-cpp CONFIG REQUIRED)` 取得系统导出的 `yaml-cpp` 目标并链接。发布记录 apt 包版本；系统库升级后复跑 M1 配置解析、错误定位和示例回归。 |
| Linux SocketCAN UAPI、线程与系统运行库 | 来自主机工具链及 Linux 开发头文件。 | 配置阶段检查 Linux x86_64 和所需 CAN 头文件，以 `find_package(Threads REQUIRED)` 接入 `Threads::Threads`；工具链和内核版本进入台架记录。 |

父仓库提交记录两个子模块的**精确提交号**。首次联网准备或使用本地镜像时运行 `git submodule update --init --recursive`；之后 configure、build、test 可断网运行。缺子模块时 CMake 指出缺失路径和初始化命令；缺系统开发包时提示运行依赖安装脚本。联合开发可显式指定本地源码路径（例如 `ROCOS_SHM_SOURCE_DIR`）；构建摘要标出实际路径、提交与工作树状态，发布按父仓库锁定的组合复现。

需要同步上游时，在独立变更中选定目标 tag/提交，审查上游差异与许可证变化，更新子模块指针及必要的本地适配，再按上表重跑对应阶段。若变更影响实际总线时序、映射或停机行为，追加受影响的 M5 台架用例。回退时一并恢复父仓库中的适配和子模块指针。 [Git 子模块文档](https://git-scm.com/docs/git-submodule)

### CMake 目标、预设与安装

最低 CMake 版本建议 3.20，启用 C 和 C++，业务代码使用 C++17，栈代码采用锁定上游支持的 C 标准。根 `CMakeLists.txt` 从 `VERSION` 读取产品版本，数字部分供 `project(VERSION ...)` 使用，终端打印含预发布后缀的全版本。依赖接入后进入 `src/`，`BUILD_TESTING=ON` 时再进入 `tests/`。源码清单显式列出，头路径、编译宏和警告选项按目标设置。

| 目标 | 编译内容与依赖 | 消费方 |
| --- | --- | --- |
| `canopennode`（静态库） | 上游选定 C 源、主站 OD.c、项目维护的 C 驱动移植。统一提供 `CO_driver_target.h`、OD 头路径及影响结构布局的宏；移植层经注册回调访问传输对象，避免反向链接主站核心库。 | `rocos_co_core` 与需要对照协议栈的测试 |
| `rocos_co_core`（项目内部静态库） | 包含配置、传输、CANopen 节点管理、主站周期、SHM 和报告；链接 `canopennode`、`rocos::shm`、yaml-cpp、`Threads::Threads`。C++ 中使用栈类型的单元继承同一套栈编译设置。 | 主站和测试共用同一份实现 |
| `rocos_co_master`（可执行文件） | 以 `src/main.cpp` 链接核心库。M1 先打通 `--help` 与 `--check-config`，M2/M3 补全正常运行路径。 | 安装到 `bin/`；源码运行使用 `build/<preset>/bin/rocos_co_master` |
| 测试可执行文件与 vcan 模拟从站 | 分别注册为 `unit`、`ipc`、`vcan` 标签；模拟从站独立进程运行。 | 默认 CTest 只运行 unit、ipc；vcan 显式启用，六轴实物按 M5 规程单独执行。 |

**构建入口。** 团队提交 schema v2 的 `CMakePresets.json`，定义同名的 debug/release 配置、构建与测试预设；建议使用 Ninja，分别输出至 `build/debug/`、`build/release/`。开发者对编译器、路径或并行度的本地覆盖放在忽略入库的 `CMakeUserPresets.json`。默认 `BUILD_TESTING=ON`，`ROCOS_CO_ENABLE_VCAN_TESTS=OFF`。 [CMake 3.20 Presets 文档](https://cmake.org/cmake/help/v3.20/manual/cmake-presets.7.html)

```sh
cmake --preset debug
cmake --build --preset debug --parallel 2
ctest --preset debug --output-on-failure
./build/debug/bin/rocos_co_master --check-config --config config/erob_6axis/co.yaml
```

**安装边界。** 使用 GNUInstallDirs，推荐前缀 `/opt/rocos/co`。交付 `bin/rocos_co_master`、`share/rocos-co/config/erob_6axis/co.yaml`、`share/doc/rocos-co/` 和第三方许可证；内部静态库、测试程序和依赖开发头文件仅用于构建。运行参数用 `--config <文件>` 显式传入，资源路径相对于声明文件解析。


## M1 · 工程、配置与六轴映像

| 任务 | 交付物 | 离线验收标准 |
| --- | --- | --- |
| 1.1 建立可重复的构建 | 按上节布局建立 CMake C/C++ 工程、debug/release presets、两个固定提交的子模块、系统 apt 包清单、版本与许可证清单。保存主站 OD 源文件、生成文件和生成器版本。 | 依赖已就位时，两种 preset 均可在断网环境从干净目录构建并运行离线测试；缺依赖或 OD 容量不足时给出具体错误。M1 的配置检查入口能独立构建并读取示例，随构建记录实际依赖提交。在独立构建目录生成产物；验证切换 preset 不污染另一构建目录，普通构建后子模块工作树保持干净。 |
| 1.2 编译六轴配置 | `--config`、`--check-config`、`--help`；完整六节点 `co.yaml`、类型化配置、每轴 PDO 表。区分 SHM ID、槽位、物理 Node-ID 和接口名。 | 静态检查不打开 CAN、不改写设备；显式 CLI 覆盖后重新校验。对重复 Node-ID/COB-ID、非字节对齐、9 字节 PDO、未知键、非法周期、超预算逐案报出配置位置。测试 50/51 个 SHM 槽、每方向 25/26 个变量和零长度方向；另验实际 OD 容量。 |
| 1.3 生成映像并建立回归基准 | 每个变量的帧内位置、SHM 绝对偏移、方向、字宽、单位来源和 COB-ID；按实际 DLC 计算的总线预算。 | 对下表逐字节断言，包括负的位置/速度和非自然对齐的 32 位值；App 字段名逐字匹配。六轴总映像 Input **66 B**、Output **42 B**，每拍 **19 帧**。 |

**独立复算基准。** 对 SHM 槽位 i=0…5，物理节点号 n 取配置值。下表为候选台架映射，部署时逐台读回确认。

| 关节视角 | COB-ID | 有效载荷 | SHM 各变量字节偏移 |
| --- | --- | --- | --- |
| 接收 RPDO1 | 0x200+n | 控制字 2 B、目标位置 4 B、模式 1 B | Output：7i + 0、2、6 |
| 发送 TPDO1 | 0x180+n | 状态字 2 B、实际位置 4 B | Input：11i + 0、2 |
| 发送 TPDO2 | 0x280+n | 实际速度 4 B、模式显示 1 B | Input：11i + 6、10 |

按设计稿的经典 CAN 标准帧填充上界，1 Mbit/s 下，六轴周期报文与空 SYNC 共 `6 × (125 + 115 + 105) + 55 = 2125 bit`，约 **2.125 ms / 8 ms = 26.56%**。这个断言约束报文计划；心跳、EMCY、重发与主机调度另计。

## M2 · 传输、协议与周期运行

| 任务 | 交付物 | 通过标准与证据 |
| --- | --- | --- |
| 2.1 接入 CANopenNode 与传输后端 | CO_driver 移植、SocketCAN、内存传输替身；主站本地 OD 与远端 PDO 配对表。 | **纯离线**：同一组帧向量覆盖过滤、长度、非阻塞收发、背压/溢出、错误帧分流与关闭；验证从站 TPDO 对应本地 RPDO。**vcan**：独立从站进程双向收发，抓包与配置逐帧相符，接收回环不被当作远端反馈。 |
| 2.2 完成节点启动及配置验证 | 有截止时间的状态机，覆盖 boot-up、NMT、双向心跳、身份 SDO、两种 PDO 策略和业务 ready 门槛。 | **离线 + vcan**：模拟已经上电、错过 boot-up、0x1017 初值为 0 三种入口。定向进入 PREOP 并核验身份；verify 只核对映射，configure 改写后逐项读回 COB-ID、有效位、类型、条数、对象和位宽。注入身份错误、SDO abort 或部分改写失败，应指出节点/对象且不发布业务 ready。维护模式关闭过程输出；op 模式须达到设定的连续完整反馈窗口。 |
| 2.3 实现周期调度和六轴故障状态机 | 单调时钟驱动的 8 ms 循环、按窗口收齐 12 个 TPDO、六帧输出暂存、组故障锁定及受控退出。输出 SYNC 编号、收齐时刻、最后目标帧和超期计数。 | **纯离线**：用可控时钟注入缺帧、错误 DLC、迟到帧和调度超期；不将不完整输入盖上新时间戳，不密集补发 SYNC，不在恢复后重放旧目标。**vcan**：抓包核对每拍 1 个 SYNC、6 个 RPDO、12 个 TPDO（按报文类型区分心跳等非周期帧）；区分线上收帧、CANopenNode 本地 OD 更新和实际发布的拍号。组故障后阻止正常目标，链路可用时保留停止动作所需的输出及 SYNC。 |

M2 留存一份正常启动抓包和一份故障抓包，能从节点号和 SYNC 拍号定位对应日志。模拟器可以制造 bus-off **事件**来测试程序分支；物理接口进入 bus-off 时的收发行为在 M5 复核。

## M3 · SHM v2、邮箱与主站输出

| 任务 | 交付物 | 通过标准与证据 |
| --- | --- | --- |
| 3.1 接入公共 ABI | 直接链接 `rocos::shm` 的 owner 与布局适配；协调在公共规范登记 co 后端代号，设计稿建议值为 3；对齐 App 的识别表。 | **纯离线跨进程**：使用 App 所锁公共头编译客户端；核对公共尺寸、IPC 名称和实际 PDO 对象长度。同 ID 第二个 owner、错误 ABI/尺寸、ready 前连接均被拒绝；验证只读监控、唯一可写 App、槽 8 和实例更换后的协调重启。公共类型代号与布局版本分别核对。 |
| 3.2 发布过程数据和诊断 | Input/Output 搬运、身份与有效位、主站心跳、周期统计和周期通知。 | **纯离线 + vcan**：首份完整 Input 先于业务首次通知；`dt=8000`，六轴逐字节读写与 M1 一致。缺必需反馈时停止正常通知及旧样本时间戳更新。NMT、CAN 错误独立报告，AL/WKC/DC 按 v2 无效或禁用语义发布；只读客户端不消费通知。 |
| 3.3 实现单命令邮箱 | 非实时命令工作线程、PREOP SDO 读写、RESET_STATS；与周期线程协调截止时间和统计复位。 | **纯离线跨进程 + vcan**：覆盖 1/64 字节成功、65 字节拒绝、读容量不足、OP 拒绝 SDO、禁止修改映射的写、旧实例、迟到响应和结果未知。响应保留有效 abort 信息；不自动重发写；RESET_STATS 只在周期边界确认。 |
| 3.4 完成 CLI 和终端报告 | 启动摘要、有效配置、SHM、逐轴 PDO、阶段失败、运行汇总、退出清理及安装产物。 | **纯离线快照 + vcan**：检查分节、单位、绝对偏移、节点与 COB-ID、覆盖来源、非 TTY 无 ANSI；错误进入 stderr 且退出码非零。检查周期正常和错误路径均无日志格式化、阻塞 SDO 或周期内堆分配。向临时前缀安装后核对交付清单，并从其他工作目录用绝对路径加载已安装示例；启动中途失败后，未 ready 的资源和通道能由本实例回收。 |

## M4 · 不接真机的 App 联调

1. **对齐部署输入。** 准备六轴模型、硬件映射、关节绑定和现有按 SHM ID 连接的 App 入口；rocos-co 单独读取六轴 co.yaml。交付一张 ID、PDO 名称、方向、字宽、换算和模型轴的对照表。台架尚未标定的物理参数注明来源与待测项。
2. **运行端到端用例。** 在 vcan 上运行独立模拟从站、候选主站、真实 App 和只读监控。给六轴各输入不同的可辨认反馈值，检查 App 读取正确轴和数值、初始目标与实际位置对齐、模式为 CSP、正常目标正确返回模拟驱动。重复正常启动、组故障与两端协调重启。用错名、错宽、错 ID 配置验证联调检查能指出不匹配；现有 App 仅告警的情况如实记录，不扩展 App 控制代码。
3. **固定送台架的版本。** 交付依赖提交、App/主站构建标识、有效配置、离线 CTest 报告和 vcan 抓包。复跑公共 `rocos_shm_v2` 和 App 的 `shared_memory_config_test`，记录结果；抽查 ECM/IgH 原有识别值和共用访问路径。进入 M5 的条件是按现有入口能稳定复现六轴闭环数据流、SHM 与抓包一致、已知影响台架试验的错误已处理。固定时序是否满足驱动器要求仍留待 M5。

拟提供的验证入口为 debug/release 的 CMake 配置与构建 preset、默认无硬件的 CTest，以及单独启用的 vcan 用例。启动六轴实物或使能驱动的用例只在指定台架显式运行。

## M5 · 必须在六轴 8 ms 台架上完成

| 任务 | 台架操作与交付物 | 验收判据 |
| --- | --- | --- |
| 5.1 确认六台实物的配置 | 在失能状态核对接线、终端、1 Mbit/s、唯一 SYNC 生产者、六个 Node-ID；保存 0x1018 身份、实际固件、心跳、PDO 通信/映射逐台读回，以及六轴与机械关节的对应关系。按附件核对实际固件支持的失联及停止行为。 | 六个实物均有读回记录；映射、位长、COB-ID、模式、实际位置和初始目标与候选配置相符。8 ms 由主站产生，设备支持情况由试验记录。将实物差异落实到配置并重跑相关离线用例后再进入六轴 CSP 试验。 |
| 5.2 验证六轴同步与一拍目标 | 在已准备的台架运行六轴 CSP。记录测试时长、周期数、负载、内核/适配器、SYNC 抖动、12 个反馈帧收齐、6 个目标帧到线、最迟目标帧距下一 SYNC 的余量；用驱动侧可观测信号核对六轴采用目标的边界。 | **功能条件**为六轴在同一 SYNC 生效。以 SYNC[k] 的反馈到六轴在 SYNC[k+1] 锁存为一拍目标；报告最坏延迟、漏拍数/总拍数、受影响轴、固定附加拍与偶发错拍。仅有总线抓包时可判定线上相位，驱动内部锁存写“未确认”。出现跨轴错拍或实测两拍时保留原始记录，分别说明同步或一拍目标未达到。 |
| 5.3 验证主站、驱动及链路失联行为 | 逐场景记录正常退出、必需 TPDO 缺帧、节点掉线或重启、仅 SYNC 消失但心跳仍在、主站异常退出、物理链路断开/bus-off。使用可控注入方式并记录注入点；留存抓包、主站日志、可取得的状态字/速度轨迹及独立测量。 | 链路可用时观测组故障锁定、正常目标停止和六轴停止动作；主站退出或断链时核验驱动本地反应。恢复需重新核验并显式使能。某轴反馈中断时，机械停稳依现场独立证据判定；缺证据标“未确认”。首轮不注入 App 生命周期事件验证联停。 |

**停机报告每个场景一行。** 取同一故障注入时刻 t0，给出 T1（检测方发现故障）、各轴 T2（驱动进入约定停止状态）、各轴 T3（实际达到本次试验约定的停稳判据），均报告相对 t0 的时间；六轴整组 T3 取最后一轴。注明检测方、时钟来源及对时误差、停止状态、速度阈值/持续时间、不可观测项。首轮先实测，暂不设停机时间上限。T2、T3 的证据与主站已发出停止请求分开记录。

## 评审与收尾

M1–M4 完成后可交付**可上台架的候选版本**。首版验收报告另列 M5 实测结果，逐项标明“达到目标 / 未达到目标 / 未确认”，附版本、配置、原始观测或日志位置及复测项。19 帧预算和 vcan 抓包不能替代六轴锁存或实际停稳的证据。

设计稿 M6 的 ZLG 适配留待后续需求，首轮只交付可替换传输接口及其测试替身。后续若要求 App 与主站联停，再由启动管理器检测 App 意外退出或明确暂停并通知 rocos-co，不在本轮引入 App 监督通道。
