// ============================================================================
// 最小化 Legacy 协议 RPC 服务端 — 用于真实链路 QPS 压测
// ============================================================================
// 无需 muduo / ZK，纯 POSIX socket + protobuf。
// 支持多线程并发处理客户端请求。
//
// Legacy 请求格式: [4B TotalLen(BE)] [4B HeaderLen(BE)] [RpcHeader proto] [Body proto]
// Legacy 响应格式: [4B BodyLen(BE)] [Body proto]
// ============================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../example/user.pb.h"
#include "../src/protocol/rpc_header.pb.h"

static std::atomic<bool> g_running(true);

// ---- 辅助: 精确读取 N 字节 ----
ssize_t recv_exact(int fd, char* buf, size_t size) {
  size_t total = 0;
  while (total < size) {
    ssize_t n = recv(fd, buf + total, size - total, 0);
    if (n <= 0) return n;
    total += n;
  }
  return total;
}

// ---- 处理单个客户端连接 ----
void handle_client(int client_fd) {
  while (g_running.load(std::memory_order_relaxed)) {
    // 1. 读 TotalLen (4 bytes BE)
    uint32_t total_len_be = 0;
    if (recv_exact(client_fd, (char*)&total_len_be, 4) != 4) break;
    uint32_t total_len = ntohl(total_len_be);
    if (total_len == 0 || total_len > 16 * 1024 * 1024) break;

    // 2. 读 HeaderLen (4 bytes BE)
    uint32_t header_len_be = 0;
    if (recv_exact(client_fd, (char*)&header_len_be, 4) != 4) break;
    uint32_t header_len = ntohl(header_len_be);

    // 3. 读 RpcHeader + Body
    uint32_t remaining = total_len - 4;  // 减去 header_len 自身的 4 字节
    std::vector<char> frame(remaining);
    if (recv_exact(client_fd, frame.data(), remaining) != (ssize_t)remaining) break;

    // 4. 解析 RpcHeader
    xrpc::RpcHeader rpc_header;
    if (!rpc_header.ParseFromArray(frame.data(), header_len)) break;

    // 5. 解析 Body (LoginRequest)
    uint32_t body_len = remaining - header_len;
    Kuser::LoginRequest login_req;
    if (!login_req.ParseFromArray(frame.data() + header_len, body_len)) break;

    // 6. 构造 LoginResponse
    Kuser::LoginResponse login_resp;
    login_resp.mutable_result()->set_errcode(0);
    login_resp.mutable_result()->set_errmsg("ok");
    login_resp.set_success(true);

    // 7. 序列化响应
    std::string resp_body;
    if (!login_resp.SerializeToString(&resp_body)) break;

    // 8. 发送响应: [4B BodyLen(BE)] [Body]
    uint32_t resp_len_be = htonl(static_cast<uint32_t>(resp_body.size()));
    if (send(client_fd, &resp_len_be, 4, 0) != 4) break;
    if (send(client_fd, resp_body.data(), resp_body.size(), 0) !=
        (ssize_t)resp_body.size())
      break;
  }
  close(client_fd);
}

int main(int argc, char** argv) {
  int port = 9000;
  if (argc > 1) port = std::atoi(argv[1]);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, [](int) { g_running = false; });

  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }

  if (listen(listen_fd, 1024) < 0) {
    perror("listen");
    return 1;
  }

  std::cout << "[Server] Legacy protocol server listening on port " << port
            << "\n";

  std::vector<std::thread> workers;

  while (g_running) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    workers.emplace_back(handle_client, client_fd);
  }

  close(listen_fd);
  for (auto& t : workers) t.join();

  std::cout << "[Server] Shutdown complete.\n";
  return 0;
}
