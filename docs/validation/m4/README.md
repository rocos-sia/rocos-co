# M4 六轴 App 软件联调验收

候选主站 0.0.0-rc.4 已在 Ubuntu 22.04.5 上完成六轴 vcan 联调。使用真实 rocosAppMain 3.2.0-rc.12（ce59e553），CANopenNode 9b8beed8，主站和 App 的 rocos-shm 均为 14618b7d。公共规范和监控在配套提交 66e8c5d 登记 co=3，ECM=1、IgH=2。App 使用既有 Hardware、SHM ID 和通知槽 8。四方分别是主站、App、独立六节点仿真进程和只读 SHM 客户端。

## 部署和换算

主站使用[六轴 co.yaml](../../../config/erob_6axis/co.yaml)。[App 入口](../../../tests/app/erob_6axis/app.yaml)关联同目录下的 URDF、hardware.yaml 和 joint_binding.yaml。示例 SHM ID 为 2，本次运行将两端同时覆盖为 1365318，接口覆盖为 vcan0。每个正常实例的有效配置和逐轴偏移见[主站启动日志](normal.master.log)。

| 模型关节 | App drive_id / SHM 槽 | Node-ID | Input 起点 B | Output 起点 B | 初始实际和初始目标 count | 最终指令及反馈 count | App 最终位置 rad |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| joint_1 | 0 | 1 | 0 | 0 | -1000 | -900 | -0.009 |
| joint_2 | 1 | 2 | 11 | 7 | -2000 | -1800 | -0.018 |
| joint_3 | 2 | 3 | 22 | 14 | -3000 | -2700 | -0.027 |
| joint_4 | 3 | 4 | 33 | 21 | -4000 | -3600 | -0.036 |
| joint_5 | 4 | 5 | 44 | 28 | -5000 | -4500 | -0.045 |
| joint_6 | 5 | 6 | 55 | 35 | -6000 | -5400 | -0.054 |

| SHM 方向及轴内字节偏移 | PDO 变量精确名称 | 对象及类型 | 帧（n 为 Node-ID） |
| --- | --- | --- | --- |
| Input +0、+2 | Status word、Position actual value | 6041:00 u16、6064:00 i32 | 0x180+n，6 B |
| Input +6、+10 | Velocity actual value、Mode display | 606C:00 i32、6061:00 i8 | 0x280+n，5 B |
| Output +0、+2、+6 | Control word、Target Position、Mode of operation | 6040:00 u16、607A:00 i32、6060:00 i8 | 0x200+n，7 B |

仿真设定统一为 100000 count/rad、零偏 0、传动比 1。状态字与控制字按位解释，模式 8 为 CSP。试验只校验虚拟轴的读写、顺序和单位换算。实物的机械方向、零点、位置与速度系数、模型几何、限位和额定负载须由台架配置提供。

## 验收结果

- 候选版本开启 vcan 后通过 25/25 项 CTest，其中 real_app_six_axes 顺序运行六个场景；另一个 Release 构建通过全部 7/7 项默认测试。App 的 shared_memory_config_test 和公共库的 rocos_shm_v2 分别通过 1/1 项。
- 同一 SHM ID 两次协调启动，instance_id 均已记录且不同。两轮均执行获取控制、使能、MoveJ、检查最终反馈、失能和退出；六轴目标、SHM 和模拟从站接收帧一致。[结构化结果](results.json) · [正常抓包](normal.frames.txt.gz)
- 错 ID 时 App 找不到 v2 header。错名时 App 指出 drive 0 和错误名称并给出警告。错宽时 App 报 PDO 映射错误，只读描述符将差异定位至槽 0 的 Position actual value，期望 4 B，实为 2 B。
- 六轴已在仿真中使能后，让节点 5 停发 0x285 并继续生产心跳。主站非零退出，标出节点 5，六个停机状态字均可观测，该节点的速度未确认。测试随后显式结束 App。此项测量通信故障处理，机械停稳和驱动锁存待 M5 实测。[故障日志](fault.master.log) · [故障抓包](fault.frames.txt.gz)

## 复验

下列命令从 rocos-co 根目录运行。先构建相邻 rocos-app 仓库的 release 版 rocosAppMain。测试以独立网络命名空间中的 vcan0 和回环接口运行，要求宿主机支持 user/net namespace 和 vcan。

```sh
cmake -S . -B build/vcan -DROCOS_CO_ENABLE_VCAN_TESTS=ON \
    -DROCOS_CO_APP_EXECUTABLE="$PWD/../rocos-app/build/release/bin/rocosAppMain" \
    -DROCOS_CO_CLIENT_SHM_INCLUDE="$PWD/../rocos-app/3rdparty/rocos-shm/include"
cmake --build build/vcan --parallel
unshare --user --map-root-user --net sh -ec 'ip link set lo up && ip link add vcan0 type vcan && ip link set vcan0 up && ctest --test-dir build/vcan --output-on-failure'
```

每轮在 build/vcan/app-integration 中保留四方联调日志、有效 YAML 和原始帧。原始帧文本列为主机单调纳秒、模拟端观测方向、十六进制 COB-ID、十六进制数据。源码说明的生成步骤见[代码说明](../../code-overview.md)，本轮以 Doxygen 1.9.1 生成并核对了首页和主站、SHM 类文档。
