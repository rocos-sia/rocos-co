# ROCOS-CO 代码说明

从 `rocos_co_master --check-config --config config/erob_6axis/co.yaml` 开始可以离线检查配置和过程映像。正常启动时，主站独占一个 SHM ID 和一个 SocketCAN 端点。App 按该 ID 连接，读 Input，写 Output；只读监控可以同时观察。

## 从节点启动到发布反馈

1. rocos_co::MasterConfig 保存已校验的节点、PDO、字节偏移和时限。rocos_co::Transport 封装物理接口的打开、过滤和非阻塞收发。帧的主机接收时间使用单调时钟。
2. rocos_co::Commissioner 在 PREOP 推进身份核验、双向心跳配置和 PDO 映射读回。rocos_co::SdoTransfer 使用绝对截止时间，并保存从站 abort code。
3. rocos_co::MasterRuntime 推进 NMT 和 SYNC。在 OP 中，rocos_co::FeedbackWindow 按 COB-ID、DLC 和时间窗口收齐必需反馈，rocos_co::Stack 将本地主站对象字典中的值送入过程映像。
4. rocos_co::ShmOwner 在首个业务通知前初始化六轴目标及模式。正常周期间，每次完整发布都会更新 Input 和通知；输出采样时刻从 SHM 取一份副本，随后请求六帧目标发送。SHM v2 的 App 写入没有跨六轴的代号。
5. 运行故障锁定后，主站停止提交业务目标。能通信时请求组停止并记录驱动状态字和速度的可观测性。主站提交时间、总线上线时间、驱动内部锁存和机械停稳由不同观测点取得。

## 生成与查看

安装项目依赖后运行 `cmake --preset release` 和 `cmake --build build/release --target code-docs`，打开 `build/release/code-docs/html/index.html`。类索引和文件索引提供接口说明、定义位置和源代码。
