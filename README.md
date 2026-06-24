# Linux C 安全网关

这是一个面向 Linux C 网络安全数据面的学习项目。当前版本处在第 1 阶段：服务程序框架，目标是把程序先做成一个能长期运行、可配置、可观测、可优雅退出的 Linux 服务。

## 当前功能

- 从 `server.conf` 加载配置
- 支持前台运行和 `-d` 后台 daemon 模式
- 支持 `SIGTERM` / `SIGINT` 优雅退出
- 支持 `SIGHUP` 热加载配置
- 忽略 `SIGPIPE`
- 支持 DEBUG / INFO / WARN / ERROR 分级日志
- 日志写入配置文件指定的日志路径
- 启动 worker 线程并通过任务队列处理模拟任务

后续阶段会继续加入 TCP/UDP 代理、抓包分析、五元组策略匹配、Linux 网络栈实验、XDP、Suricata 协议解析、DPDK 实验和最终安全网关 Demo。

## 环境说明

当前项目在 Ubuntu WSL 中开发和测试。

安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake gdb ninja-build pkg-config libnet1-dev
```

如果使用 CLion 2024.3.x + Ubuntu 26.04，建议使用已安装的兼容工具版本：

```text
CMake: /usr/local/bin/cmake
GDB:   /usr/local/bin/gdb
```

## 构建

在 WSL 中执行：

```bash
cd /mnt/d/code/linux-c-security-gateway
/usr/local/bin/cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
/usr/local/bin/cmake --build cmake-build-debug --target linux_c_security_gateway -- -j 4
```

CMake 会把 `server.conf` 复制到构建目录，因此从 `cmake-build-debug` 目录运行时，默认的 `./server.conf` 可以被正常找到。

如果需要构建 echo 测试工具：

```bash
/usr/local/bin/cmake --build cmake-build-debug --target echo_test -- -j 4
```

## 运行

从构建目录前台运行：

```bash
cd /mnt/d/code/linux-c-security-gateway/cmake-build-debug
./linux_c_security_gateway
```

指定配置文件运行：

```bash
./linux_c_security_gateway -c /mnt/d/code/linux-c-security-gateway/server.conf
```

后台 daemon 模式运行：

```bash
./linux_c_security_gateway -c /mnt/d/code/linux-c-security-gateway/server.conf -d
```

查看帮助：

```bash
./linux_c_security_gateway -h
```

## TCP/UDP Echo 测试工具

`tools/echo_test.c` 用来测试服务端 TCP echo 和 UDP echo 是否正常。默认连接 `127.0.0.1:8080`，并依次测试 TCP 和 UDP。

先启动服务端：

```bash
cd /mnt/d/code/linux-c-security-gateway/cmake-build-debug
./linux_c_security_gateway
```

在另一个终端运行默认测试：

```bash
cd /mnt/d/code/linux-c-security-gateway/cmake-build-debug
./echo_test
```

默认测试等价于：

```bash
./echo_test -M both -a 127.0.0.1 -p 8080 -s "hello echo" -n 1 -T 3000
```

单独测试 TCP echo：

```bash
./echo_test -M tcp -a 127.0.0.1 -p 8080 -s "hello tcp" -n 3
```

单独测试 UDP echo：

```bash
./echo_test -M udp -a 127.0.0.1 -p 8080 -s "hello udp" -n 3
```

参数说明：

```text
-M    测试协议：tcp、udp、both，默认 both
-a    服务端地址，默认 127.0.0.1
-p    服务端端口，默认 8080
-s    发送内容，默认 "hello echo"
-n    测试次数，默认 1
-T    接收超时时间，单位毫秒，默认 3000
-h    查看帮助
```

返回码说明：

```text
0     测试通过
1     连接、收发或回显内容校验失败
2     参数错误
```

当前 `server.conf` 默认同时监听 `tcp,0.0.0.0,8080,echo` 和 `udp,0.0.0.0,8080,echo`，因此默认测试可以直接覆盖 TCP/UDP echo。

## 配置文件

默认配置文件：

```text
server.conf
```

示例：

```conf
listen_ip=0.0.0.0
listen_port=8080
worker_num=4
log_level=info
log_file=/tmp/myserver.log
```

支持的日志级别：

```text
debug, info, warn, warning, error
```

## 日志

默认日志文件：

```bash
/tmp/myserver.log
```

查看日志：

```bash
tail -f /tmp/myserver.log
```

每条日志包含时间、日志级别、进程 ID、线程 ID、源码文件和行号。

## 信号测试

启动程序：

```bash
cd /mnt/d/code/linux-c-security-gateway/cmake-build-debug
./linux_c_security_gateway
```

查找进程：

```bash
pgrep -af linux_c_security_gateway
```

热加载配置：

```bash
kill -HUP <pid>
```

优雅退出：

```bash
kill -TERM <pid>
```

预期行为：

- `SIGHUP` 会重新加载 `server.conf`，并重新打开日志文件。
- `SIGTERM` 或 `SIGINT` 会停止任务生成，唤醒 worker 线程，等待线程退出，释放资源并结束进程。

## CLion 配置说明

使用 WSL 工具链，不是 SSH 工具链。

推荐路径：

```text
CMake:      /usr/local/bin/cmake
C 编译器:   /usr/bin/gcc
C++ 编译器: /usr/bin/g++
调试器:     /usr/local/bin/gdb
```

如果运行时报：

```text
invalid config path: ./server.conf
```

检查 CLion 的运行配置：

- Working directory 可以设置为构建目录：
  `/mnt/d/code/linux-c-security-gateway/cmake-build-debug`
- 或者在 Program arguments 中显式传入：
  `-c /mnt/d/code/linux-c-security-gateway/server.conf`

Windows 访问项目目录时，直接打开：

```text
D:\code\linux-c-security-gateway
```

不要从 Windows 资源管理器访问：

```text
\\wsl$\Ubuntu\mnt\d\...
```

## 当前验收状态

第 1 阶段当前状态：

- [x] CMake 构建成功
- [x] 支持配置文件加载
- [x] 支持前台启动
- [x] 支持 daemon 启动路径
- [x] 支持文件日志
- [x] worker 线程可启动和退出
- [x] 支持 `SIGTERM` / `SIGINT` 优雅退出逻辑
- [x] 支持 `SIGHUP` reload 逻辑
- [ ] 补充 README 命令输出作为验收证据
- [ ] 补充 ASan 运行记录
- [ ] 补充 Valgrind 运行记录
- [ ] 使用 `lsof` 检查 fd 是否持续增长
- [ ] 增加前台日志输出，不只写文件

## 推荐仓库结构

学习路线建议的目录结构：

```text
00_docs/
01_service_framework/
02_tcp_udp_proxy/
03_packet_analysis/
04_policy_engine/
05_linux_network_stack/
06_xdp_demo/
07_suricata_parser/
08_dpdk_lab/
final_gateway_demo/
```

## 下一步

建议先把第 1 阶段收口：

1. 在 `00_docs/` 下补充阶段验收记录。
2. 跑一次 ASan 或 Valgrind，并保存输出。
3. 用 `lsof` 做 fd 检查记录。
4. 开始 `02_tcp_udp_proxy`：先实现基于非阻塞 epoll 的 TCP echo server。
