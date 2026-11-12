# ROCOS-CO 开发约定

面向 Linux x86_64 的 CANopen 主站，业务代码使用 C++17，CANopenNode 移植层使用 C。阶段和验收项见 [实施计划](docs/canopen-master-implementation-plan.md)，协议与配置合同见 [设计方案](docs/canopen-master-design.md)。

- 先初始化两个锁定的子模块，再运行 `scripts/install-deps-ubuntu-22.04.sh` 安装系统开发包。避免修改第三方工作树。修改主站 OD 时同时提交 XDD、生成的 C/H，并检查生成工具版本。
- 使用 `cmake --preset debug`、`cmake --build --preset debug`、`ctest --preset debug --output-on-failure`。release 使用同名 release 预设。构建产物留在 build/。配置检查不依赖 CAN 接口。
- src/ 按协议、传输、配置、SHM 与生命周期分工。YAML 示例放入 config/，类和函数按需写 Doxygen 注释，说明时序、单位、所有权和失败语义。配置与诊断在周期外执行。
- SHM 布局、原子访问与 IPC 所有权使用 rocos-shm v2。App 通过 SHM ID 连接。涉及 App 的额外行为需要项目负责人确认。
- 按 M1–M5 顺序验收，区分纯离线、真实 SocketCAN/vcan 联调与六轴实物证据。仅在专用且已准备好的台架运行硬件用例。
- 每次提交前执行 ROCOS-App 的 rocos-version-maintenance skill。当前系列固定 0.0.0，每个验收提交递增 rc.N。VERSION 是本项目产品版本的唯一来源，构建生成的头文件从该文件派生。
