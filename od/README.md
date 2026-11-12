# 主站对象字典

master.xdd 派生自锁定版本 CANopenNode 的 example/DS301_profile.xpd（Apache-2.0）。主站可以接收 12 个 PDO、发送 6 个 PDO，心跳消费者容量 50。0x2000–0x200B 对应接收缓冲，0x2100–0x2105 对应发送缓冲。每个镜像数组有 8 个独立字节，允许现场配置改变 PDO 内的变量宽度与名字。部署时从 co.yaml 编译出的映射决定实际 DLC。镜像索引属于主站本地，远端驱动对象使用 YAML 中的索引。

构建使用已提交的 OD.c 和 OD.h。修改 XDD 后，使用 CANopenEditor **v4.2.3** 官方发布包中的 net8.0/EDSSharp.dll 和 .NET 运行时重新生成。发布包 SHA-256 为 `efe970d1812f08fc8ed8f20edc2891301fce376b464a9840f58df3ccd1627c15`。[上游发布包](https://github.com/CANopenNode/CANopenEditor/releases/tag/v4.2.3)

```sh
EDSSHARP_DLL=/path/to/net8.0/EDSSharp.dll ./scripts/generate-od.sh
git diff -- od/master.xdd od/generated
```
