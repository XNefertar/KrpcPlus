# Xrpc 多核压测会话迁移上下文

> 更新时间：2026-08-27  
> 工作区：`/data/workspace`  
> 目标项目：`/data/workspace/Xrpc`

## 1. 原始目标

用户希望确认并实现：针对 Xrpc 项目，在正式 `RpcServer` 中进行真实多核压测；随后要求实际执行构建和压测命令、分析结果，并估算同环境下 tRPC-Cpp 的峰值 QPS。

## 2. 初始调研结论

项目已有以下基础：

- 正式服务端入口：`example/callee/server.cc`，产物为 `bin/server`。
- 正式服务端实现：`src/rpc/server.cc`、`src/include/xrpc/rpc/server.h`。
- 真实直连压测客户端：`example/caller/bench_client.cc`，产物为 `bin/bench_client`。
- 压测客户端每个线程持有独立 `RpcChannel`，可指定服务端地址、线程数、每线程请求数和超时。
- 原服务端通过 Muduo 工作线程运行，但 Worker 数硬编码为 4。
- 原示例业务在每次请求时打印日志，会严重干扰性能。
- 原服务端全局共享单个 `_serverCodec`，通过 `std::call_once` 初始化，不适合多连接、多协议、多 Worker 场景。
- 正式服务端启动依赖 ZooKeeper 注册服务。
- 原 `run_benchmark.py` 启动多个服务端进程，不是单个正式服务端的多核伸缩测试。

## 3. 已完成的源码修改

当前 Git 中有 6 个已修改文件，尚未提交：

```text
M example/callee/CMakeLists.txt
M example/callee/server.cc
M src/CMakeLists.txt
M src/common/application.cc
M src/include/xrpc/rpc/server.h
M src/rpc/server.cc
```

另有未跟踪的依赖构建目录：

```text
?? .benchmark-deps/
```

### 3.1 Worker 数配置化

修改 `src/common/application.cc`：

- 新增命令行参数 `-t <threads>`。
- `-t` 写入配置项 `rpcserverthreads`。
- 当前帮助格式：

```text
command -i <配置文件路径> [-a <ip>] [-p <port>] [-t <threads>]
```

修改 `src/rpc/server.cc`：

- 优先读取 `rpcserverthreads`。
- 未配置时使用 `std::thread::hardware_concurrency()`。
- 最少为 1。
- 非法值捕获异常并回退到硬件线程数。
- 启动日志打印实际 Worker 数：`workers:<n>`。

使用示例：

```bash
./bin/server -i /tmp/xrpc-bench/test.conf -t 8
```

### 3.2 Codec 改为连接级状态

修改 `src/include/xrpc/rpc/server.h` 和 `src/rpc/server.cc`：

- 删除全局 `_serverCodec` 和 `codec_init_flag_`。
- 增加：

```cpp
std::mutex connection_codecs_mutex_;
std::unordered_map<std::string, std::shared_ptr<ServerCodec>> connection_codecs_;
```

- 每条 TCP 连接首次收到数据时自动检测协议并创建独立 Codec。
- 连接断开时从表中删除 Codec。
- 异步响应回调捕获当前连接的 `shared_ptr<ServerCodec>`，保证生命周期安全。
- 请求解码和响应编码均使用当前连接的 Codec。

注意：目前每次 `OnMessage` 仍会获取全局 `connection_codecs_mutex_` 并查询连接表。这可能是 8→16 Worker 扩展效率下降的热点。后续最好把 Codec 直接放入 Muduo 连接 Context，消除请求路径上的全局互斥锁。

### 3.3 移除逐请求日志

修改 `example/callee/server.cc`：

- `Login` 和 `Register` 不再逐请求输出到 `std::cout`。
- 参数改为常量引用并使用 `(void)` 消除未使用警告。

### 3.4 构建修复

修改 `example/callee/CMakeLists.txt`：

- 正式服务端改为链接 `xrpc_server_core ${SERVER_LIBS}`。
- 编译标准由 C++11 改为 C++17，以适配当前 Protobuf。

修改 `src/CMakeLists.txt`：

- 将 `src/include/xrpc/protocol/*.pb.cc` 加入 `xrpc_core`，解决协议 Protobuf 未定义符号。

修改 `src/rpc/server.cc`：

- 增加 `#include <google/protobuf/message.h>`，解决不完整类型问题。

此外，为完成构建，执行过 Protobuf 生成：

```bash
protoc --cpp_out=/data/workspace/Xrpc/example \
  -I/data/workspace/Xrpc/example \
  /data/workspace/Xrpc/example/user.proto

mkdir -p /data/workspace/Xrpc/src/include/xrpc/protocol
protoc --cpp_out=/data/workspace/Xrpc/src/include/xrpc/protocol \
  -I/data/workspace/Xrpc/src/protocol \
  /data/workspace/Xrpc/src/protocol/rpc_header.proto \
  /data/workspace/Xrpc/src/protocol/xrpc_protocol.proto
```

这些生成文件当前没有出现在 `git status` 中，推测被 `.gitignore` 忽略；迁移后如清理构建目录或换机器，需要重新生成或调整 CMake 自动生成流程。

## 4. 环境准备与依赖状态

系统最初没有 CMake，已安装并用于 Release 构建。还安装了 Protobuf、glog、gflags、Boost、Ant、JDK、CppUnit 等系统依赖。

由于软件源没有可直接使用的 Muduo 和 ZooKeeper C 客户端，源码依赖位于：

```text
/data/workspace/Xrpc/.benchmark-deps/
```

当前约 117 MB。主要内容包括：

- Muduo 源码和构建产物。
- ZooKeeper 多个尝试版本；最终使用 ZooKeeper 3.4 分支构建 C 客户端。
- ZooKeeper C 客户端安装目录。

为兼容旧 ZooKeeper 与当前 JDK/GCC，在临时依赖源码中做过调整：

- Java source/target 从 1.6 调到 1.7。
- 移除 `Record.java` 中仅用于标注的 Yetus audience annotation。
- 绕过 CppUnit Autoconf 宏。
- 使用 `-Wno-error -Wno-format-overflow` 构建。
- 重新启用 sync/threaded API，生成 `libzookeeper_mt.a`。

Muduo、ZooKeeper C 客户端静态库及头文件被复制到 `/usr/local/lib` 和 `/usr/local/include`。这是工作区外的系统级改动。

最终 Release 构建成功，产物当前存在：

```text
/data/workspace/Xrpc/bin/server       约 3.98 MB
/data/workspace/Xrpc/bin/client       约 453 KB
/data/workspace/Xrpc/bin/bench_client 约 441 KB
```

构建命令：

```bash
cmake -S /data/workspace/Xrpc \
  -B /data/workspace/Xrpc/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build /data/workspace/Xrpc/build -j8
```

存在一个非阻断警告：

```text
Protobuf compiler version 24.2 doesn't match library version 4.24.2
```

本次编译、链接和累计数百万次 RPC 均成功，未观察到运行错误。

## 5. ZooKeeper 与临时配置

压测使用 Docker ZooKeeper：

```text
容器名：xrpc-zk-bench
状态：当前仍在运行
端口：宿主机 2181 -> 容器 2181
镜像：zookeeper:3.9
```

启动命令：

```bash
docker run -d --rm --name xrpc-zk-bench \
  -p 2181:2181 zookeeper:3.9
```

临时正式服务端配置：

```text
/tmp/xrpc-bench/test.conf
```

内容：

```ini
rpcserverip=127.0.0.1
rpcserverport=8000
rpcserverthreads=4
zookeeperip=127.0.0.1
zookeeperport=2181
```

命令行 `-t` 会覆盖配置中的 `rpcserverthreads`。

## 6. 实际压测方法

机器报告 `nproc=64`。

为了减少客户端与服务端争抢：

- 服务端绑定 CPU `0-15`。
- 客户端绑定 CPU `16-31`。
- 服务端 Worker 分别测试 `1/2/4/8/16`。
- 客户端使用 64 个线程/长连接。
- 每线程 20,000 次请求。
- 每轮总请求 1,280,000。
- 单次请求超时 1000 ms。
- 正式 `RpcServer`、真实 TCP loopback、Protobuf RPC、空业务逻辑。

服务端命令模板：

```bash
taskset -c 0-15 ./bin/server \
  -i /tmp/xrpc-bench/test.conf \
  -t <worker_count>
```

客户端命令：

```bash
taskset -c 16-31 ./bin/bench_client \
  127.0.0.1 8000 64 20000 1000
```

完整日志位于：

```text
/tmp/xrpc-bench/
```

长测客户端日志命名类似：

```text
client-1-long.log
client-2-long.log
client-4-long.log
client-8-long.log
client-16-long.log
```

## 7. 实测结果

所有长测：

- 成功率 100%。
- 失败请求 0。
- 每轮 1,280,000 请求。

| Worker | Overall QPS | 耗时 | 相对 1 Worker 加速 | 单 Worker 并行效率 |
|---:|---:|---:|---:|---:|
| 1 | 99,645.4 | 12.846 s | 1.00x | 100% |
| 2 | 170,654.4 | 7.501 s | 1.71x | 85.6% |
| 4 | 258,823.4 | 4.945 s | 2.60x | 64.9% |
| 8 | 445,741.0 | 2.872 s | 4.47x | 55.9% |
| 16 | 526,872.5 | 2.429 s | 5.29x | 33.0% |

16 Worker 完整关键指标：

```text
Total Requests: 1280000
Completed:      1280000
Success:        1280000
Fail:           0
Elapsed:        2.429 s
Success Rate:   100.00%
Overall QPS:    526872.5 req/s
Peak QPS:       907554.1 req/s
```

扩展变化：

- 1→2 Worker：约 +71.3%。
- 2→4 Worker：约 +51.7%。
- 4→8 Worker：约 +72.2%。
- 8→16 Worker：约 +18.2%。

结论：

- 多核改造已生效。
- 当前最高稳态约 52.69 万 QPS。
- 秒级 Peak 约 90.76 万 QPS。
- 8 Worker 前扩展明显，16 Worker 开始边际递减。
- 当前较好的性价比点约在 8 Worker。

短测曾使用每线程 2,000 次、总计 128,000 请求，结果明显受连接建立和预热影响：

| Worker | 短测 QPS |
|---:|---:|
| 1 | 58,301 |
| 2 | 77,218 |
| 4 | 89,931 |
| 8 | 104,411 |
| 16 | 111,170 |

因此正式报告应采用长测，不应采用短测。

## 8. 性能瓶颈判断

当前 8→16 Worker 仅提升约 18.2%，可能原因：

1. `OnMessage` 每次都竞争 `connection_codecs_mutex_`。
2. 客户端只有 64 个连接，可能不足以完全压满 16 Worker。
3. 客户端和服务端仍处于同一物理机，尽管已分核，仍共享 LLC、内存带宽和内核 TCP/IP loopback。
4. 测试只持续数秒，仍不够稳定；正式压测建议预热 10 秒、持续至少 30～60 秒。
5. 每请求创建 `MuduoConnection`、`ProtocolMessage`、请求对象、响应对象和 `FunctionClosure`，动态分配较多。
6. 业务方法同步运行在 I/O Worker，真实耗时业务可能阻塞 Reactor。
7. `conn->send(out.ToString())` 存在连续 Buffer 转字符串的复制。
8. 客户端统计和同步等待机制也可能先达到上限。

推荐后续优化顺序：

1. 把 Codec 放到 Muduo `TcpConnection::Context`，移除全局 map 和 mutex。
2. 增加 128/256/512 连接档位，或使用 2～4 个客户端进程。
3. 增加固定时长模式、预热阶段和 P50/P95/P99/P999。
4. 服务端与客户端分两台物理机，并使用真实网卡测试。
5. 使用 `perf stat`、`perf record`、火焰图确认锁、分配、复制和系统调用占比。
6. 考虑请求/响应/Closure 对象池与 Buffer 复用。

## 9. 对 tRPC-Cpp 的估算

用户询问同环境下 tRPC-Cpp 的峰值 QPS。此前给出的估算并非实测，结论是：

| 指标 | Xrpc 实测 | tRPC-Cpp 估算 |
|---|---:|---:|
| 稳态 Overall QPS | 526,873 | 70万～95万 |
| 最可能稳态 | 526,873 | 约 82万 |
| 秒级 Peak QPS | 907,554 | 100万～130万 |
| 最可能峰值 | 907,554 | 约 115万 |
| 稳态相对提升 | — | 约 1.3～1.8 倍 |

中心估计：

```text
tRPC-Cpp 秒级 Peak QPS ≈ 115 万
合理区间 ≈ 100 万～130 万 QPS
```

估算依据：tRPC-Cpp 通常具有更成熟的 Reactor/Fiber 调度、连接状态局部化、对象池、缓冲区复用、低竞争队列和编解码路径；而当前 Xrpc 请求路径有全局 Codec map mutex 和较多动态分配。

重要限制：

- 这是工程估算，不是 tRPC-Cpp 实测。
- 没有找到可直接与本环境严格对齐的官方 benchmark 数据。
- 64 连接的单客户端可能先达到上限。
- 公平对比必须统一 Proto、请求/响应大小、连接数、线程数、绑核、超时、预热和统计口径。

推荐公平测试矩阵：

- 服务端 Worker：16。
- 服务端绑核：`0-15`。
- 客户端绑核：`16-31`。
- 连接数：64、128、256。
- 预热：10 秒。
- 正式采样：30～60 秒。
- 指标：Overall QPS、1 秒 Peak、P50/P95/P99/P999、客户端/服务端 CPU、上下文切换、每核 QPS。

## 10. 当前工作区注意事项

1. 源码修改尚未提交。
2. `.benchmark-deps/` 是未跟踪目录，约 117 MB；不要误认为项目正式源码。
3. `.benchmark-deps/` 不应直接提交，建议加入 `.gitignore` 或移到项目外，但尚未执行。
4. ZooKeeper 容器 `xrpc-zk-bench` 仍在运行。
5. `/tmp/xrpc-bench/` 中的配置和日志是临时文件，重启或清理 `/tmp` 后可能消失。
6. 已向 `/usr/local` 复制 Muduo/ZooKeeper 头文件和静态库。
7. Protobuf 生成文件可能被忽略；更稳妥的后续改造是让 CMake 自动调用 `protoc`。
8. 正式压测目前还是本机 loopback，不等价于跨机器真实网络。
9. 当前 `service_map` 在启动后只读，通常安全；如果未来支持运行时动态注册，需要同步保护。
10. `receive_time` 当前未使用。

## 11. 建议迁移后的下一步

若继续优化 Xrpc，建议新模型按以下顺序进行：

1. 检查 `git diff`，确认现有修改符合预期。
2. 将连接 Codec 移入 Muduo Connection Context，去除全局互斥锁。
3. 增强 `bench_client`：固定时长、预热、延迟分位数、并发连接与线程解耦。
4. 重新运行 1/2/4/8/16 Worker 长测，比较锁移除前后结果。
5. 使用 `perf` 分析 16 Worker 热点。
6. 如需比较 tRPC-Cpp，应搭建同 Proto、同业务、同 CPU 配额的真实基准，而不是继续依赖估算。

## 12. 可直接复用的关键命令

确认状态：

```bash
git -C /data/workspace/Xrpc status --short
docker ps --filter name=xrpc-zk-bench
```

重新构建：

```bash
cmake -S /data/workspace/Xrpc \
  -B /data/workspace/Xrpc/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build /data/workspace/Xrpc/build -j8
```

启动 16 Worker 正式服务端：

```bash
cd /data/workspace/Xrpc
taskset -c 0-15 ./bin/server \
  -i /tmp/xrpc-bench/test.conf \
  -t 16
```

执行长测：

```bash
cd /data/workspace/Xrpc
taskset -c 16-31 ./bin/bench_client \
  127.0.0.1 8000 64 20000 1000
```

停止临时 ZooKeeper：

```bash
docker stop xrpc-zk-bench
```

只有确认不再需要压测环境时再执行停止命令。
