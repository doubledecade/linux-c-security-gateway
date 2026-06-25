# 第二阶段差距说明

生成日期：2026-06-25

本文档根据 `Linux_C网络安全数据面学习路线_融合版_含验收标准.docx` 中“阶段 2：Linux 网络编程”的任务和验收标准，对当前仓库代码状态做静态对比。

## 结论

当前第二阶段已经有 TCP/UDP echo、TCP proxy、UDP proxy 的功能雏形，并且主程序已切换到 `02_tcp_udp_proxy` 目录实现；但还不能认为第二阶段已经验收完成。

主要原因是：当前实现只把 listener fd 放进 epoll，accept 后的 TCP client 又被改回阻塞模式并交给 worker 处理；长连接或 TCP proxy 连接会长期占用 worker，距离“基于 epoll 的非阻塞多连接模型”和“100 个 TCP 连接稳定运行”的优秀标准还有差距。同时，仓库缺少并发测试脚本、tcpdump/pcap 抓包证据、fd 泄漏检查记录和阶段验收记录。

## 学习文档要求

阶段 2 的目标是：独立实现 TCP/UDP 网络服务、代理和转发程序，掌握 epoll、非阻塞 IO 和异常连接处理。

基础通过标准：

- TCP echo：10 个客户端同时连接不崩。
- UDP 转发：能把 client 数据转发到 upstream，并能返回响应。
- TCP 代理：client 和 upstream 能双向通信。
- 非阻塞：send/recv 遇到 EAGAIN 不退出、不死循环。
- epoll：不使用一个连接一个线程的方式。
- 日志：每条连接有创建、关闭、错误日志。

优秀标准：

- 并发测试：100 个 TCP 连接稳定运行。
- 超时处理：空闲连接能自动关闭。
- 异常关闭：client/upstream 任意一端断开，另一端能正确回收。
- fd 泄漏：压测后 fd 数不持续增长。
- 抓包证明：tcpdump 能证明代理转发链路。

必须提交的东西：

- `02_tcp_udp_proxy/tcp_echo_server`
- `02_tcp_udp_proxy/udp_forwarder`
- `02_tcp_udp_proxy/tcp_proxy`
- 并发测试脚本
- tcpdump 抓包文件
- README

## 当前已经具备

1. 主程序已经接入第二阶段代码。

   `CMakeLists.txt` 当前把 `linux_c_security_gateway` 链接到 `02_tcp_udp_proxy/config.c`、`log.c`、`main_service.c`。

2. 配置支持多个 listener。

   `server.conf` 支持如下形式：

   ```conf
   listener=tcp,0.0.0.0,8080,echo
   listener=udp,0.0.0.0,8080,echo
   listener=tcp,0.0.0.0,18080,proxy,127.0.0.1,20000
   listener=udp,0.0.0.0,18080,proxy,127.0.0.1,8080
   ```

3. 已有基础 TCP/UDP echo 处理。

   TCP echo 在 worker 中读取 client 数据并原样写回。UDP echo 收到报文后用 `sendto` 回包。

4. 已有基础 TCP proxy。

   `process_tcp_proxy_client` 会连接 upstream，然后通过 `poll` 在 client 和 upstream 之间双向转发。

5. 已有基础 UDP proxy。

   `process_udp_proxy_packet` 会把 UDP 数据发给目标地址，等待 upstream 响应，再把响应发回原 client。

6. 已有测试工具。

   - `tools/echo_test.c`：测试 TCP/UDP echo。
   - `proxy_test`：复用 `echo_test`，默认测试 `18080` 端口。
   - `tools/target_echo_server.c`：作为独立 upstream echo server。

7. README 已写了基本运行命令。

   README 包含 echo 测试、proxy 测试和独立 upstream 测试流程。

## 当前主要缺口

### 1. 第二阶段验收记录缺失

README 里的“当前验收状态”仍是第 1 阶段，且“下一步”仍写着开始 `02_tcp_udp_proxy`。仓库之前没有 `00_docs/`、第二阶段验收记录、测试输出记录或问题复盘。

需要补充：

- 阶段 2 验收记录。
- 编译命令输出。
- echo/proxy 测试输出。
- 并发测试输出。
- tcpdump 抓包命令和 pcap 文件说明。
- fd/ASan/Valgrind 检查记录。

### 2. TCP echo 还不是真正的 epoll 非阻塞多连接模型

当前主线程只把监听 socket 放入 epoll。accept 到 client 后，代码会调用 `set_blocking(client_fd)`，然后把 client fd 投递给 worker。worker 内部对这个 client 做阻塞式 `recv` 循环。

影响：

- 一个长连接会占住一个 worker。
- 并发能力主要受 `worker_num` 限制。
- 这不满足学习文档里“epoll：不使用一个连接一个线程的方式”的精神。
- 100 个 TCP 长连接稳定运行很难证明。

建议改进：

- client fd 保持非阻塞。
- 把 client fd 也加入 epoll。
- 为每个连接维护状态结构：fd、peer、last_active、in_buf、out_buf、service_type、target_fd。
- 用事件驱动处理 `EPOLLIN` / `EPOLLOUT` / `EPOLLERR` / `EPOLLHUP`。

### 3. TCP proxy 会长期占用 worker

当前 TCP proxy 在 worker 内部使用：

```c
poll(fds, 2, -1);
```

这表示一个 proxy 连接在关闭前会一直占住一个 worker。

影响：

- 4 个 worker 默认只能稳定处理 4 条长时间存在的 TCP proxy 会话。
- 连接空闲不会自动超时关闭。
- 无法满足“100 个 TCP 连接稳定运行”和“空闲连接能自动关闭”的优秀标准。

建议改进：

- 把 client fd 和 upstream fd 都放入 epoll。
- 建立 proxy session 状态，维护 client/upstream 两端 fd。
- 支持双向缓冲和 `EPOLLOUT` 回写。
- 增加连接空闲超时扫描。
- 对 FIN、RST、半关闭、POLLHUP/EPOLLHUP 做明确日志和资源回收。

### 4. UDP 转发缺少五元组、包数、字节数统计

学习文档要求 UDP 转发器支持监听地址和后端地址配置，并记录五元组、包数、字节数。

当前 UDP proxy 能完成单包请求和响应，但没有维护统计表。

需要补充：

- UDP 五元组记录：src_ip、src_port、dst_ip、dst_port、protocol。
- 每个 flow 的 packet_count 和 byte_count。
- 总包数、总字节数、错误数。
- 定期或退出时输出统计日志。

### 5. 并发测试脚本缺失

当前 `echo_test` 的 `-n` 是同一个 TCP 连接内顺序发送多次，不是多个客户端同时连接。它不能证明“10 个客户端同时连接”和“100 个 TCP 连接稳定运行”。

需要补充并发测试脚本，例如：

- `tools/tcp_concurrency_test.py` 或 `tools/tcp_concurrency_test.sh`
- 同时启动 10 个 TCP echo client。
- 同时启动 100 个 TCP echo client。
- 同时启动多条 TCP proxy client。
- 输出成功数、失败数、平均耗时、错误原因。

### 6. tcpdump / pcap 证据缺失

学习文档要求 tcpdump 证明 `client -> proxy -> upstream` 三段链路。

当前仓库没有：

- pcap 文件。
- tcpdump 命令记录。
- 抓包结果说明。
- Wireshark 字段说明。

建议新增：

- `00_docs/tcpdump_reports/stage2_tcp_proxy.md`
- `00_docs/tcpdump_reports/stage2_udp_proxy.md`
- `00_docs/pcap/` 保存抓包文件，或在文档里说明 pcap 文件较大时如何生成。

### 7. fd 泄漏、ASan、Valgrind 记录缺失

阶段 2 优秀标准要求压测后 fd 数不持续增长。

当前仓库没有：

- `lsof -p <pid>` 前后对比。
- `/proc/<pid>/fd` 数量记录。
- ASan 运行记录。
- Valgrind 运行记录。

建议补充：

- `00_docs/stage2_fd_check.md`
- `00_docs/stage2_asan_valgrind.md`

### 8. README 和当前配置存在不一致

README 中说明默认 TCP proxy 转发到 `127.0.0.1:8080`，但当前 `server.conf` 中 TCP proxy 目标是 `127.0.0.1:20000`。

当前配置：

```conf
listener=tcp,0.0.0.0,18080,proxy,127.0.0.1,20000
listener=udp,0.0.0.0,18080,proxy,127.0.0.1,8080
```

影响：

- 按 README 直接运行 `proxy_test` 可能失败，除非额外启动了 `127.0.0.1:20000` 的 TCP upstream。
- TCP proxy 和 UDP proxy 的默认 upstream 不一致，容易造成测试误判。

建议：

- 要么把 README 改成当前配置。
- 要么把 `server.conf` 改回 TCP/UDP 都转发到 `127.0.0.1:8080`。
- 更推荐保留 `server_proxy_only.conf` 专门做独立 upstream 测试，默认 `server.conf` 先保持 echo/proxy 最小可跑。

### 9. SIGHUP reload 不会重建 listener

当前 SIGHUP reload 后会重新加载配置和日志，但已经创建的 `channels[]` 仍是启动时的 listener 配置。

影响：

- 修改 listener 端口不会新增/关闭监听 socket。
- 修改 proxy target 后，已有 `channels[]` 中的 target 配置不会自动更新。
- README 或验收文档不能声称第二阶段 listener/proxy 配置已经完全热加载生效。

建议：

- 阶段 2 可以先明确限制：SIGHUP 只 reload 日志和全局配置，不重建 listener。
- 如果要完整实现，需要对比新旧 listener 配置，新增/删除/更新 epoll 中的 listener fd。

## 建议收口顺序

1. 先修正 README 与 `server.conf` 的 proxy upstream 不一致问题。
2. 新增第二阶段验收记录模板，记录当前已通过和未通过项。
3. 增加并发测试脚本，先证明 10 个 TCP echo client 同时连接。
4. 重构 TCP echo：client fd 进入 epoll，保持非阻塞。
5. 增加连接空闲超时和 fd 回收日志。
6. 重构 TCP proxy 为 epoll session 模型，避免一个长连接占一个 worker。
7. 给 UDP proxy 增加五元组、包数、字节数统计。
8. 跑 100 TCP 并发测试，并保存输出。
9. 用 tcpdump 抓 TCP proxy 和 UDP proxy 链路，保存 pcap 和报告。
10. 做 fd 增长检查、ASan 或 Valgrind 记录。

## 当前验收判断

| 验收项 | 当前判断 | 说明 |
| --- | --- | --- |
| TCP echo | 部分完成 | 功能雏形存在，但并发和 epoll 非阻塞模型不足 |
| UDP 转发 | 部分完成 | 能转发回包，但缺少五元组/包数/字节数统计 |
| TCP 代理 | 部分完成 | 能双向转发基础流量，但连接占 worker，缺少空闲超时和完整异常处理证据 |
| 非阻塞 | 部分完成 | listener/UDP socket 非阻塞，但 TCP client 被改回阻塞 |
| epoll | 部分完成 | 只 epoll listener fd，未 epoll 管理连接 fd |
| 日志 | 部分完成 | 有创建/关闭/错误日志，但还缺系统化验收记录 |
| 10 客户端并发 | 未证明 | 缺少并发测试脚本和输出 |
| 100 TCP 连接 | 未完成/未证明 | 当前模型不适合证明 100 长连接稳定运行 |
| 空闲超时 | 未完成 | TCP echo/proxy 没有连接级 idle timeout |
| 异常关闭 | 部分完成 | 有基本关闭处理，但缺 client/upstream 任意一端断开的测试证据 |
| fd 泄漏检查 | 未完成 | 缺少 lsof 或 `/proc/<pid>/fd` 记录 |
| 抓包证明 | 未完成 | 缺少 tcpdump 命令、pcap 和报告 |
| README | 部分完成 | 有基本命令，但阶段状态和当前配置需要更新 |

总体状态：第二阶段处于“功能雏形完成，验收证据和网络模型收口不足”的状态。
