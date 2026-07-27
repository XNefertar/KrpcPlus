# Xrpc

高性能分布式 RPC 框架，基于 C++17 编写。

## 特性

- **双协议兼容**：同一端口同时支持 Legacy 和 XRpc 两种协议，通过 Magic Number (`0x4B52`) 自动识别，实现业务方零改动的平滑协议迁移
- **高性能网络模型**：服务端基于 Muduo 网络库的多 Reactor 模型，取代传统 thread-per-connection 模型
- **服务发现与负载均衡**：基于 ZooKeeper 实现服务注册与发现，支持 RoundRobin 和 Random 两种负载均衡策略
- **RCU 无锁路由缓存**：读路径使用 `atomic_load<shared_ptr>` 零锁读取，写路径使用 Copy-on-Write + `atomic_store` 原子替换
- **全链路可观测性**：9 阶段耗时统计 + 固定桶直方图，支持 P50/P95/P99 分位数计算、瓶颈诊断和长尾告警
- **Protobuf 生态兼容**：RPC 调用接口兼容 Protobuf Service 体系，可直接使用 protoc 生成的 Stub 代码

## 依赖

| 依赖 | 用途 |
|------|------|
| C++17 | 语言标准 |
| CMake >= 3.5 | 构建系统 |
| Protobuf | 协议序列化 |
| ZooKeeper | 服务注册与发现 |
| Muduo | 服务端网络库 |
| Glog + Gflags | 日志系统 |
| Boost | 工具库 |

## 快速开始

### 安装依赖

Linux/Ubuntu 环境下可参考 `build_rpc_1.sh` 一键安装：

```bash
# 基础工具
sudo apt-get install -y wget cmake build-essential unzip

# ZooKeeper
sudo apt-get install -y zookeeperd libzookeeper-mt-dev

# Protobuf
sudo apt-get install -y protobuf-compiler libprotobuf-dev

# Glog & Gflags
sudo apt-get install -y libgoogle-glog-dev libgflags-dev

# Boost
sudo apt-get install -y libboost-all-dev

# Muduo (需从源码编译安装)
# 请参照 Muduo 官方文档编译安装
```

### 构建

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

构建产物输出到 `bin/` 目录：

| 产物 | 说明 |
|------|------|
| `libxrpc_core.a` | 客户端核心静态库 |
| `libxrpc_server_core.a` | 服务端核心静态库 |
| `server` | 示例服务端 |
| `client` | 示例客户端（ZK 模式） |
| `bench_client` | 直连压测客户端 |

### 运行示例

**1. 启动 ZooKeeper**

```bash
sudo service zookeeper start
```

**2. 启动服务端**

```bash
./bin/server -i test.conf
```

**3. 发起调用**

```bash
# ZK 模式（自动服务发现）
./bin/client -i test.conf

# 直连模式（跳过 ZK）
./bin/bench_client 127.0.0.1 8000
```

### 配置文件

```ini
# RPC 服务端配置
rpcserverip=127.0.0.1
rpcserverport=8000

# ZooKeeper 配置
zookeeperip=127.0.0.1
zookeeperport=2181

# 负载均衡策略 (roundrobin / random)
loadbalancer=roundrobin
```

命令行参数：
- `-i <config_file>` 指定配置文件
- `-a <ip>` 覆盖配置中的 IP
- `-p <port>` 覆盖配置中的端口

## 使用示例

### 定义服务

```protobuf
syntax="proto3";
package example;
option cc_generic_services=true;

message LoginRequest {
  bytes name = 1;
  bytes pwd = 2;
}
message LoginResponse {
  int32 errcode = 1;
  bool success = 2;
}

service UserServiceRpc {
  rpc Login(LoginRequest) returns(LoginResponse);
}
```

### 服务端

```cpp
#include "xrpc/rpc/server.h"

class UserService : public example::UserServiceRpc {
  void Login(::google::protobuf::RpcController* controller,
             const ::example::LoginRequest* request,
             ::example::LoginResponse* response,
             ::google::protobuf::Closure* done) override {
    response->set_errcode(0);
    response->set_success(true);
    done->Run();
  }
};

int main(int argc, char** argv) {
  Application::Init(argc, argv);
  RpcServer provider;
  provider.NotifyService(new UserService());
  provider.Run();  // 阻塞等待
}
```

### 客户端

```cpp
#include "xrpc/rpc/channel.h"
#include "xrpc/rpc/controller.h"

// ZK 模式
example::UserServiceRpc_Stub stub(new RpcChannel(false));
// 直连模式
example::UserServiceRpc_Stub stub(new RpcChannel("127.0.0.1", 8000, false));

example::LoginRequest request;
request.set_name("zhangsan");
request.set_pwd("123456");

example::LoginResponse response;
Controller controller;

stub.Login(&controller, &request, &response, nullptr);

if (controller.Failed()) {
  std::cout << "RPC failed: " << controller.ErrorText() << std::endl;
} else {
  std::cout << "Success: " << response.success() << std::endl;
}
```

## 协议帧格式

### Legacy 协议

```
┌────────────┬────────────┬──────────┬──────┐
│ TotalLen   │ HeaderLen  │ RpcHeader│ Body │
│ (4B BE)    │ (4B BE)    │ (Proto)  │      │
└────────────┴────────────┴──────────┴──────┘
```

### XRpc 新协议

```
┌──────────────────────────────────────┬──────────┬──────┐
│ FixedHeader (16B)                    │ VarHeader│ Body │
│  Magic(2B) + Type(1B) + Stream      │ (Proto)  │      │
│  Type(1B) + TotalSize(4B) +         │          │      │
│  HeaderSize(2B) + StreamID(4B) +    │          │      │
│  Reserved(2B)                       │          │      │
└──────────────────────────────────────┴──────────┴──────┘
```

## 项目结构

```
Xrpc/
├── CMakeLists.txt              # 顶层构建配置
├── build_rpc_1.sh              # 依赖一键安装脚本
├── run_benchmark.py            # 自动化压测脚本
├── test_multi_method.py        # 多方法测试脚本
├── docs/
│   ├── 技术深度剖析与面试指南.md
│   └── 测试与压测报告.md
├── src/
│   ├── codec/                  # 双协议编解码器
│   ├── common/                 # 框架基础设施 (Application, Config, Logger)
│   ├── monitor/                # 全链路耗时监控
│   ├── protocol/               # Protobuf 定义与生成代码
│   ├── registry/               # ZooKeeper 客户端与路由管理
│   └── rpc/                    # RPC 核心 (Server, Channel, Controller)
├── example/                    # 使用示例
└── tools/                      # 压测与基准测试工具
```

## 监控

框架内置 9 阶段全链路耗时统计：

| 阶段 | 说明 |
|------|------|
| `ZK_QUERY` | ZooKeeper 服务发现耗时 |
| `LOAD_BALANCE` | 负载均衡选择节点耗时 |
| `CONNECT` | TCP 连接建立耗时 |
| `SERIALIZE_REQ` | 请求序列化耗时 |
| `ENCODE_REQ` | 请求编码耗时 |
| `NET_IO` | 网络 I/O 耗时 |
| `DECODE_RES` | 响应解码耗时 |
| `DESERIALIZE_RES` | 响应反序列化耗时 |
| `TOTAL` | 端到端总耗时 |

## 压测工具

```bash
# 自动化压测
python3 run_benchmark.py

# QPS 模拟压测
./tools/qps_benchmark

# RCU vs RwLock vs Mutex 对比
./tools/rcu_benchmark

# 真实 TCP 全链路压测
./tools/real_benchmark
```

## 许可证

[GNU General Public License v3.0](LICENSE)
