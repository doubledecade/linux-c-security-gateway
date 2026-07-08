# Linux C 网络安全数据面学习路线（实战重构版）

> 版本：2026-07-04  
> 目标：把“知道 socket、epoll、TCP/IP 概念”转化为“能独立实现、测试、定位和讲解网络数据面程序”。  
> 主线：Linux C 网络编程 → 流式协议解析 → 安全网关 → Linux 网络栈/XDP → Suricata。  
> 选修：DPDK。  
> 建议投入：每周 12～15 小时；低于 8 小时按 8～10 个月执行。

---

## 1. 这份计划的关键调整

1. **先完成网络代码闭环，再补工程框架。** 不再先花数周完善 daemon、配置系统和通用线程池。
2. **前 6 周只解决 epoll、连接状态、缓冲区、背压和资源释放。** 暂不加入 TLS、FEC、HA、磁盘缓存。
3. **每个连接必须有唯一所有者。** socket 状态只能由所属 reactor 线程修改，禁止“一连接一线程”。
4. **每周必须同时提交代码、自动测试和证据。** 只有代码能跑不算完成。
5. **Suricata 新应用层协议扩展使用 Rust。** 独立流式解析器仍使用 C；Suricata 8 官方要求新 app-layer protocol 使用 Rust。
6. **DPDK 改为选修。** 安全网关和协议解析主线未完成前，不进入 DPDK。

---

## 2. 当前起点

仓库已有：

- Linux C 服务框架、配置和日志雏形；
- TCP/UDP echo；
- TCP/UDP proxy 功能雏形；
- 基础测试工具。

当前最关键的缺口：

- TCP listener 使用 epoll，但 accept 后的 client fd 又被改为阻塞模式；
- TCP echo/proxy 长连接会占住 worker；
- TCP proxy 仍使用阻塞 `poll`，不是完整 reactor；
- 缺少连接输入/输出缓冲区、部分写处理和背压上限；
- 缺少半关闭、RST、连接超时和非阻塞 connect 状态机；
- 缺少并发、fd、ASan、pcap 等验收证据；
- UDP 缺少完整 flow key、会话超时和统计。

因此，不从头重写。第一个里程碑是把现有 `02_tcp_udp_proxy` 收口成真正的非阻塞 reactor。

---

## 3. 必须建立的核心心智模型

### 3.1 TCP 是字节流

- 一次 `send` 不对应一次 `recv`；
- `recv` 可能只得到半条消息，也可能一次得到多条消息；
- `send` 可能只发送一部分；
- `EAGAIN` 表示当前没有进展条件，不是连接错误；
- epoll 通知的是“fd 现在可能可读/可写”，不是“业务消息到达”。

### 3.2 每条连接都必须有上下文

最低限度的数据结构：

```c
typedef enum {
    CONN_CONNECTING,
    CONN_ESTABLISHED,
    CONN_HALF_CLOSED,
    CONN_CLOSING,
    CONN_CLOSED
} conn_state_t;

typedef struct endpoint {
    int fd;
    bool read_closed;
    bool write_closed;
    buffer_t input;
    buffer_t output;
} endpoint_t;

typedef struct proxy_session {
    uint64_t id;
    conn_state_t state;
    endpoint_t client;
    endpoint_t upstream;
    uint64_t last_active_ms;
    uint64_t bytes_c2s;
    uint64_t bytes_s2c;
} proxy_session_t;
```

### 3.3 reactor 线程只做五件事

```text
epoll_wait
  → 根据 data.ptr 找到连接或会话
  → 读取直到 EAGAIN
  → 解析/转发并写入对端输出缓冲区
  → 根据缓冲区状态调整 EPOLLIN/EPOLLOUT
  → 发生错误或状态结束时统一释放资源
```

### 3.4 所有权规则

- listener 只负责创建连接；
- 一个 `proxy_session` 只归一个 reactor；
- reactor 负责 epoll、fd、buffer、timer 和 session 生命周期；
- 其他线程不能直接关闭 reactor 管理的 fd；
- 跨线程操作通过队列加 `eventfd` 投递；
- 配置 reload 使用不可变快照或双缓冲切换。

---

## 4. 目标工程结构

```text
linux-c-security-gateway/
├── 00_docs/
│   ├── architecture.md
│   ├── weekly_reviews/
│   ├── packet_analysis/
│   ├── performance/
│   └── pcap/
├── 01_service_framework/
├── 02_tcp_udp_proxy/
│   ├── reactor.c/.h
│   ├── connection.c/.h
│   ├── buffer.c/.h
│   ├── timer.c/.h
│   ├── tcp_proxy.c/.h
│   ├── udp_flow.c/.h
│   └── tests/
├── 03_stream_parser/
├── 04_policy_engine/
├── 05_gateway_core/
├── 06_linux_network_stack/
├── 07_xdp_demo/
├── 08_suricata_parser/
├── 09_dpdk_lab/                 # 选修
├── tools/
└── final_gateway_demo/
```

约束：

- 单个 `.c` 文件尽量不超过 800 行；
- 网络协议结构不能直接把 C struct 原样发送；
- 线协议字段必须明确字节序、长度、版本和最大值；
- 所有 fd 必须能追踪到创建和关闭位置；
- 所有动态内存必须有明确所有者。

---

## 5. 24 周路线总览

| 阶段 | 周期 | 核心目标 | 主要产物 |
|---|---:|---|---|
| A. 非阻塞网络闭环 | 第 1～6 周 | 真正掌握 epoll、状态机、缓冲区、背压和释放 | TCP echo、TCP proxy、UDP forwarder、自动验收 |
| B. 流式协议与抓包 | 第 7～9 周 | 掌握半包/粘包/非法输入和网络证据分析 | C 流式 parser、pcap 报告 |
| C. 安全网关核心 | 第 10～12 周 | 五元组策略、热加载、审计和网关集成 | policy engine、gateway v1 |
| D. Linux 网络栈与 XDP | 第 13～15 周 | 理解转发/NAT/conntrack/XDP 路径 | netns 实验、XDP 黑名单 |
| E. Suricata/DPI | 第 16～20 周 | 理解 Flow/Transaction/Parser/EVE/Alert | Rust app-layer parser、EVE 和规则 |
| F. 综合验收 | 第 21～24 周 | 故障、稳定性、性能和表达收口 | final gateway、压测与架构文档 |
| G. DPDK | 第 25～28 周 | 选修高性能收发包 | helloworld、l2fwd、对比报告 |

进入下一阶段的条件不是“时间到了”，而是当前阶段的必须验收项全部通过。

---

## 6. 阶段 A：非阻塞网络闭环（第 1～6 周）

### 第 1 周：非阻塞 TCP echo

#### 编码任务

- 保留现有配置和日志模块；
- accept 后的 client fd 保持 `O_NONBLOCK`；
- listener fd 和 client fd 都加入 epoll；
- 为每个 client 创建 `connection_t`；
- 使用 LT 模式起步；
- `recv` 循环读取到 `EAGAIN`；
- EOF、EPOLLERR、EPOLLHUP 统一进入关闭路径。

#### 必须验收

- 100 个并发长连接能够同时保持；
- 单连接连续发送 10000 次结果正确；
- 不存在“一条连接占用一个 worker”；
- 每条连接日志包含 connection id、peer、创建和关闭原因；
- 测试结束后 `/proc/<pid>/fd` 恢复到基线附近。

### 第 2 周：输出缓冲区、部分写和背压

#### 编码任务

- 实现可增长但有上限的 `buffer_t`；
- `send` 未完成的数据保存在 output buffer；
- output 非空才监听 `EPOLLOUT`；
- output 清空后移除 `EPOLLOUT`；
- output 超过高水位时暂停对端 `EPOLLIN`；
- 降至低水位时恢复读取；
- 增加连接空闲超时。

#### 必须验收

- 客户端每次只读取 1 字节，服务端内存不会无限增长；
- 人工制造 `EAGAIN` 后连接不退出、不忙循环；
- 单连接发送大于 socket buffer 的数据，回显内容和顺序正确；
- 空闲连接按配置超时关闭。

### 第 3 周：非阻塞 TCP proxy

#### 编码任务

- client 和 upstream 都进入同一个 reactor；
- upstream 使用非阻塞 `connect`；
- 通过 `EPOLLOUT + getsockopt(SO_ERROR)` 判断连接完成；
- 一个 session 管理 client/upstream 两个 endpoint；
- 实现 C2S、S2C 两个方向的独立缓冲区；
- 删除“一连接一个 poll/线程”的实现。

#### 必须验收

- `client ↔ proxy ↔ upstream` 双向通信；
- upstream 不存在、拒绝连接、连接超时都有明确行为；
- 100 个 proxy 长连接同时保持；
- 慢 upstream 不导致进程内存无限增长。

### 第 4 周：FIN、RST、半关闭和统一生命周期

#### 编码任务

- 区分 EOF、RST、超时、主动关闭和程序退出；
- 收到一端 FIN 后，允许已缓存数据发送完成；
- 缓冲区清空后对另一端执行 `shutdown(SHUT_WR)`；
- 设计幂等的 `session_close()`；
- 任意错误路径都不能二次 close 或 use-after-free。

#### 必须验收

- client 主动 FIN；
- upstream 主动 FIN；
- client 使用 RST 退出；
- upstream 使用 RST 退出；
- 两端同时关闭；
- 所有场景下 fd 和 session 都被释放。

### 第 5 周：UDP forwarder 和 flow 表

#### 编码任务

- flow key 至少包含：协议、源 IP/端口、目的 IP/端口、通道标识；
- 每个 flow 记录包数、字节数、创建时间、最后活动时间；
- 实现 flow 超时和回收；
- 上游响应能映射回原客户端；
- 明确限制 flow 最大数量；
- 避免只用 `src_ip + src_port` 作为会话键。

#### 必须验收

- 同一客户端 socket 访问两个目标不会串流；
- 多客户端并发请求都能得到正确响应；
- 超时 flow 会释放；
- 日志输出完整五元组、packet count、byte count；
- 使用 `tc netem` 制造丢包、延迟和乱序并记录现象。

### 第 6 周：阶段 A 自动化验收

#### 必须提交

- `tcp_concurrency_test`；
- `tcp_slow_reader_test`；
- `tcp_half_close_test`；
- `tcp_rst_test`；
- `udp_multi_flow_test`；
- `fd_leak_test.sh`；
- TCP/UDP proxy pcap；
- ASan/UBSan 结果；
- 阶段验收报告。

#### 阶段 A 通过标准

| 项目 | 必须标准 |
|---|---|
| 并发 | 100 个长连接稳定；1000 个连接建立/关闭循环成功 |
| 正确性 | 大数据、1 字节分段、双向转发内容一致 |
| 背压 | 慢接收端不会导致无限内存增长 |
| 异常 | FIN/RST/拒绝连接/超时都有自动测试 |
| 资源 | 压测后 fd 不持续增长；ASan/UBSan 无错误 |
| CPU | 空闲和 EAGAIN 场景无忙循环 |
| 证据 | README、命令、日志、pcap、验收报告齐全 |

未通过阶段 A 时，不进入策略引擎、XDP 或 Suricata。

---

## 7. 阶段 B：流式协议解析与抓包（第 7～9 周）

### 第 7 周：C 流式协议解析器

协议格式：

```text
| MAGIC(4) | VERSION(1) | TYPE(1) | LENGTH(4, network order) | PAYLOAD(N) |
```

要求：

- 使用显式序列化/反序列化，禁止直接发送 packed struct；
- 校验 MAGIC、VERSION、TYPE、LENGTH；
- LENGTH 有配置上限；
- 一次输入支持 0、1、多个完整 PDU；
- 支持任意位置切分形成的半包；
- 错误输入有明确错误码；
- parser 不依赖 socket，可独立单元测试。

必须测试：

- 正常单包；
- 两包粘连；
- 每次输入 1 字节；
- header 半包；
- payload 半包；
- 错误 MAGIC/版本/类型；
- 长度为 0、超限和整数溢出；
- 随机输入不崩溃。

### 第 8 周：parser 接入 proxy

- 在 TCP proxy 中增加可选协议观察模块；
- parser 只观察，不改变透明转发结果；
- 解析结果输出结构化 JSON；
- 每条 flow 维护独立 parser state；
- 限制单 flow 的 parser 缓冲区和 transaction 数量。

### 第 9 周：抓包与故障定位

至少完成以下实验：

1. TCP 三次握手；
2. 正常 FIN；
3. RST；
4. 重传与 Dup ACK；
5. Zero Window 或慢接收端；
6. MTU/MSS；
7. UDP 丢包/乱序；
8. NAT 前后地址变化。

每份报告包含：拓扑、复现命令、抓包命令、关键字段、现象、结论、与代码的对应关系。

---

## 8. 阶段 C：安全网关核心（第 10～12 周）

### 第 10 周：五元组策略引擎

支持：

- IPv4 精确地址和 CIDR；
- 单端口、端口范围、any；
- TCP/UDP/ICMP；
- allow/deny；
- rule id、priority；
- default deny；
- 明确的首条命中或最高优先级语义。

最低测试：50 条表驱动测试，包括网段边界、端口边界、优先级冲突和默认规则。

### 第 11 周：并发查询与热加载

- 解析新配置时构建新的不可变 policy snapshot；
- 校验成功后原子切换；
- 旧 snapshot 等待读者退出后释放；
- reload 失败时保留旧策略；
- 多 reactor 查询期间反复 reload，不崩溃、不出现空策略窗口。

### 第 12 周：gateway v1

- TCP/UDP 流量在建立或首包阶段查询策略；
- deny 不建立 upstream；
- audit JSON 包含时间、五元组、rule id、action、原因；
- 输出连接数、flow 数、包数、字节数、错误数；
- pcap 证明 allow 转发和 deny 阻断。

阶段通过标准：策略测试全部通过，并发 reload 压测无崩溃，审计和抓包结果一致。

---

## 9. 阶段 D：Linux 网络栈与 XDP（第 13～15 周）

### 第 13 周：network namespace 拓扑

使用 netns/veth 建立：

```text
client namespace ↔ gateway namespace ↔ server namespace
```

完成路由、`ip_forward`、ARP、转发和抓包验证。

### 第 14 周：Netfilter/NAT/conntrack

- INPUT/FORWARD/OUTPUT；
- PREROUTING/POSTROUTING；
- SNAT/DNAT/MASQUERADE；
- conntrack NEW/ESTABLISHED 观察；
- 故意制造路由缺失、NAT 错误和防火墙阻断。

### 第 15 周：XDP 黑名单

- 解析 Ethernet/VLAN/IPv4/TCP/UDP 时严格做边界检查；
- 支持 XDP_PASS/XDP_DROP；
- BPF map 保存 IP 黑名单和统计；
- 用户态工具动态增删地址；
- 对比 socket gateway、iptables 和 XDP 的处理位置。

本阶段不追求极限 PPS，重点是正确解释包处理路径和安全边界。

---

## 10. 阶段 E：Suricata / DPI（第 16～20 周）

> 固定一个明确的 Suricata 8.x 版本，构建、开发和测试全部使用同一 tag。  
> 官方说明：新的应用层协议代码只使用 Rust。参考：<https://docs.suricata.io/en/suricata-8.0.1/devguide/extending/app-layer/overview.html>

### 第 16 周：使用与架构

- 使用 `suricata -r` 处理 pcap；
- 理解 packet、flow、stream、app-layer、detect、output 链路；
- 阅读 EVE flow/alert 日志；
- 用现有协议规则完成一次告警实验。

### 第 17 周：Rust 最小准备与协议脚手架

- 只学习实现 parser 必需的 Rust：结构体、enum、Result、slice、所有权、FFI；
- 使用官方脚本或同版本示例创建 app-layer 协议骨架；
- 注册协议探测、TCP parser、State 和 Transaction。

### 第 18 周：流式解析与异常事件

- 将第 7 周 C parser 的协议语义迁移到 Rust；
- 支持一次输入多个 PDU；
- 正确返回 incomplete；
- 限制 transaction 数量和 payload 大小；
- 非法 MAGIC、VERSION、TYPE、LENGTH 设置协议事件。

### 第 19 周：EVE JSON 与检测

- 输出自定义协议 transaction 日志；
- 注册必要的检测字段或 frame；
- 规则命中异常 TYPE/字段；
- `suricata -r test.pcap` 后同时产生协议日志和 alert。

### 第 20 周：测试与文档

- normal、sticky、partial、magic_error、length_error、gap pcap；
- parser 单元测试；
- EVE 输出样例；
- Flow、State、Transaction、Frame、Alert 关系图；
- 说明 C 独立 parser 与 Suricata Rust parser 的边界。

---

## 11. 阶段 F：综合验收（第 21～24 周）

### 第 21 周：架构收口

最终网关核心只包含：

- reactor；
- connection/session；
- buffer/backpressure；
- timer；
- TCP/UDP forwarding；
- policy snapshot；
- audit/metrics；
- config reload。

XDP 是前置可选组件，Suricata 是分析组件，不强行编进同一个进程。

### 第 22 周：故障注入

自动覆盖：

- client/upstream FIN、RST；
- upstream 拒绝、超时、DNS/配置错误；
- 慢读、慢写；
- UDP 丢包、延迟、乱序；
- reload 成功和失败；
- 日志目录不可写；
- fd 上限较低；
- SIGTERM 优雅退出。

### 第 23 周：性能与稳定性

记录：

- 并发连接数；
- 新建连接速率；
- TCP 吞吐；
- UDP PPS；
- p50/p95/p99 延迟；
- CPU、RSS、fd；
- 丢包和错误数；
- 运行 24 小时资源趋势。

性能测试必须记录机器配置、编译参数、消息大小、连接数和工具命令，禁止只写一个吞吐数字。

### 第 24 周：文档与表达

必须产出：

- 一页模块图；
- 一页数据流图；
- 一页线程和所有权图；
- 一页 TCP session 状态机；
- 一页压测报告；
- 一页典型故障抓包报告；
- 一页设计取舍；
- README 一键复现流程；
- 10 个项目面试问题及答案。

---

## 12. 阶段 G：DPDK 选修（第 25～28 周）

只有以下条件全部满足才进入：

- 阶段 A～F 已通过；
- 能解释 socket、XDP 和 DPDK 收包路径差异；
- 目标岗位明确要求 DPDK；
- 有独立 Linux 环境和可安全绑定的网卡。

任务：

1. hugepage、EAL、lcore；
2. mbuf/mempool；
3. RX/TX queue 与 port；
4. 跑通并解释 l2fwd；
5. 记录 PPS、CPU、丢包；
6. 编写 socket/XDP/DPDK 适用场景对比。

不要把“跑通示例”描述成“掌握 DPDK 数据面开发”。

---

## 13. 每周执行模板

建议每周 12～15 小时：

| 类型 | 占比 | 说明 |
|---|---:|---|
| 编码 | 45% | 核心功能和异常路径 |
| 自动测试 | 25% | 功能、故障、资源、边界 |
| 抓包/调试 | 15% | tcpdump、Wireshark、gdb、strace |
| 阅读 | 10% | 只读当前问题所需资料 |
| 文档 | 5% | 记录结论和证据，不写长篇摘抄 |

每周结束必须回答：

```markdown
# Week N 验收记录

## 本周目标
## 完成的代码
## 未完成项
## 编译与运行命令
## 自动测试结果
## 抓包/性能/资源证据
## 本周最重要的三个问题
## 我现在能独立解释什么
## 下一周进入条件
```

---

## 14. 统一质量门槛

### 编译

```text
-Wall -Wextra -Wpedantic -Wshadow -Wconversion
Debug: -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
Release: -O2 -g
```

暂时无法开启的 warning 必须逐项说明，禁止整体关闭告警。

### 测试

- 单元测试：buffer、parser、policy、timer；
- 集成测试：TCP/UDP proxy；
- 故障测试：FIN/RST/超时/拒绝连接/慢读；
- 资源测试：fd、RSS、ASan/UBSan；
- 网络证据：pcap；
- 性能测试：固定环境、可重复命令。

### 完成定义

一个功能只有同时满足以下条件才算完成：

1. 代码可编译；
2. 自动测试可重复；
3. 异常路径有测试；
4. 无已知 fd/内存泄漏；
5. README 写明运行命令；
6. 有日志、pcap 或指标证明；
7. 能脱离代码讲清数据流、状态和所有权。

---

## 15. 暂停和降级规则

- 某阶段连续两周未通过：缩小功能，不增加新模块；
- proxy 尚未正确处理部分写：禁止开始策略引擎；
- parser 尚未通过半包/粘包测试：禁止接入 Suricata；
- gateway 尚未完成资源验收：禁止进入 DPDK；
- 时间不足时，删除 DPDK，不删除网络基础和协议解析；
- 不为了“进度”保留无法解释的复制代码。

---

## 16. 如何利用现有 f_recv/f_send 代码学习

不要从头通读，也不要直接复制。完成阶段 A 后按问题阅读：

1. 对比 `f_recv` 的 listener、连接分发和内部 worker；
2. 对比 `f_send/IoThread.c` 的 epoll 与 eventfd 投递；
3. 分析 `TaskInfo` 如何维护乱序状态；
4. 分析 `TcpConnect` 的连接复用和重试；
5. 写一份“值得保留的设计”和“不能照搬的问题”。

重点识别：

- 单线程状态所有权为什么减少锁；
- 一连接一 connect 线程为什么破坏扩展性；
- packed struct 作为线协议为什么脆弱；
- U 端提前返回 HTTP 200 为什么不是端到端可靠性；
- UDP 会话键为什么必须包含完整业务维度。

---

## 17. 从现在开始的 7 天

### Day 1

- 画出现有 TCP echo 的 fd、线程和函数调用图；
- 找出 accept 后改回阻塞模式的位置；
- 定义 `connection_t` 和关闭责任。

### Day 2

- client fd 保持非阻塞；
- client fd 加入 epoll；
- 实现 EPOLLIN 读取到 EAGAIN。

### Day 3

- 实现 output buffer；
- 正确处理部分写；
- output 非空时注册 EPOLLOUT。

### Day 4

- 实现 EOF、EPOLLERR、EPOLLHUP 的统一关闭；
- 日志记录 connection id 和关闭原因。

### Day 5

- 编写 10/100 并发客户端测试；
- 编写 1000 次连接建立/关闭测试。

### Day 6

- 检查 `/proc/<pid>/fd`；
- 运行 ASan/UBSan；
- tcpdump 保存 echo pcap。

### Day 7

- 修复测试发现的问题；
- 写 Week 1 验收记录；
- 只有全部通过才进入背压和超时。

---

## 18. 最终能力标准

完成路线后，应能独立完成并解释：

- 从 socket 创建到释放的完整生命周期；
- epoll LT/ET、EAGAIN 和部分写；
- TCP proxy 的双端状态、半关闭和背压；
- UDP flow 的身份、映射、超时和统计；
- TCP 字节流上的半包、粘包和协议状态机；
- 五元组策略的线程安全热加载；
- 用 pcap 证明网络行为；
- Linux 转发、Netfilter、conntrack 和 XDP 的处理位置；
- Suricata Flow、State、Transaction、Parser、EVE 和 Alert；
- 用测试、指标和资源证据说明程序不是“碰巧能跑”。

最终目标不是记住更多 API，而是看到任何网络功能时，能够立即回答：

```text
数据从哪里来？
归哪个线程和对象管理？
当前状态是什么？
数据不完整或暂时发不出去时放在哪里？
什么时候恢复处理？
错误时谁负责释放？
如何用测试、日志和抓包证明行为正确？
```
