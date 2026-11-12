# M2 协议与周期验收

在临时 Linux 网络命名空间中建立 vcan0，主站运行真实 SocketCAN 后端和 CANopenNode。独立 Python 从站进程维护六套 NMT、SDO 对象字典、心跳定时及同步 PDO 缓冲。主站侧测试程序提供一份内存过程映像，后续 M3 接入 SHM。测试运行与主机网络隔离。

```sh
cmake --preset debug -B build/vcan -DROCOS_CO_ENABLE_VCAN_TESTS=ON
cmake --build build/vcan --parallel 4
unshare --user --map-root-user --net sh -ec 'ip link add vcan0 type vcan && ip link set vcan0 up && ctest --test-dir build/vcan --output-on-failure'
```

共 6 项离线和 14 项 vcan 用例。离线检查负值与非对齐映像、CANopenNode 本地 PDO 与全部字节镜像、传输背压、软件待发清理及可控时间窗口。vcan 检查独立进程收发、自己的回环、标准帧过滤、冷/热启动、configure 与 verify、维护模式、身份不符、部分 SDO 写失败、映射读回不符、漏帧、DLC 错误、EMCY、重启和 bus-off 事件。模拟接口没有物理位速率，预算使用配置的 1 Mbit/s；物理链路错误及机械响应待 M5 验证。

- **正常运行**。12 个反馈 PDO 在当前 SYNC 后通过事件驱动的主站本地 RPDO 进入 OD，收齐后发布当前输入。主站在两个 SYNC 之间采样目标，再经本地 TPDO 发出六帧。检查每个含正常目标的窗口都有 12 帧反馈和 6 个互异的目标 ID。[正常抓包](cold.frames.txt.gz) · [主站摘要](cold.log)
- **第 5 轴反馈丢失**。模拟从站自第 24 拍开始不再发送 0x285，主站锁定故障。故障后的目标只包含受控停止请求，继续 SYNC 并限时观测。状态字和速度按轴分别记录。掩码最低位对应槽 0，速度掩码 47 表示缺少槽 4。观察限期用于软件退出，项目规定的物理停机时间由台架测量。[故障抓包](missing.frames.txt.gz) · [主站摘要](missing.log)

每条抓包记录依次为模拟从站进程观察时的单调时间 ns、发送方、十六进制 COB-ID 和数据。主站摘要中的 feedback_ns 是 SocketCAN 收包时间，od_ns 是最近一次完整映像写入时间，last_target_ns 是主机最近一次提交目标的时间，三者独立更新。时间相位与故障在节点/主站进程的观察范围内解释。用例执行后所有场景的完整文件在 build/vcan/traces/。
