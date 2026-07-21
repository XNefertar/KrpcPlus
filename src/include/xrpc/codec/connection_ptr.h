#pragma once

#include <memory>
#include <string>

// ============================================================================
// Connection / ClientContext — Codec 层轻量抽象
// ============================================================================
// 说明：
//   Codec 层不依赖 Muduo / 具体传输实现, 只持有类型擦除的指针。
//   实际使用时 wrapper 层负责将 Muduo::TcpConnectionPtr 映射为 ConnectionPtr。

struct Connection {
  virtual ~Connection() = default;
};
using ConnectionPtr = std::shared_ptr<Connection>;

// ---- 客户端上下文 ----
struct ClientContext {
  std::string service_name;
  std::string method_name;
  std::string caller;
  std::string caller_app;
  uint32_t    timeout_ms = 1000;
};
using ClientContextPtr = std::shared_ptr<ClientContext>;

