// ============================================================================
// Xrpc 高性能服务端 — 多 Reactor + poll() I/O 多路复用 (v2 正确版)
// ============================================================================
// 架构:
//   Main Thread:   accept() → RoundRobin 分发给 Worker
//   Worker ×N:     poll() 监听分配的 fd，非阻塞处理请求
//
// 与原始 mini_server 对比:
//   原始: 每连接一个线程 (thread-per-connection)
//   新版: N 个 Worker 线程，各自 poll() 管理 M 个连接 (M:N 多路复用)
//
// 编译:
//   g++ -std=c++17 -O2 -pthread tools/mini_server_reactor.cc \
//       example/user.pb.cc src/protocol/rpc_header.pb.cc \
//       -I . -I src/include \
//       -I/opt/homebrew/opt/protobuf/include -I/opt/homebrew/opt/abseil/include \
//       -L/opt/homebrew/opt/protobuf/lib -lprotobuf \
//       -L/opt/homebrew/opt/abseil/lib -labsl_log_internal_check_op \
//       -o /tmp/mini_server_reactor
// ============================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "example/user.pb.h"
#include "src/protocol/rpc_header.pb.h"

// ---- 常量 ----
constexpr int kMaxFds = 4096;
constexpr int kMaxFrameSize = 16 * 1024 * 1024;

static std::atomic<bool> g_running(true);

// ---- 设置非阻塞 ----
void SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ---- 处理的单个请求 (阻塞式读，因为 localhost + 小消息，简化实现) ----
bool ProcessOneRequest(int fd) {
  // 读帧头 8 字节: [4B TotalLen][4B HeaderLen]
  uint8_t header_buf[8];
  size_t received = 0;
  while (received < 8) {
    ssize_t n = recv(fd, header_buf + received, 8 - received, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return true;  // 数据未到，不是错误
      return false;
    }
    received += n;
  }

  uint32_t total_len = ntohl(*(uint32_t*)header_buf);
  uint32_t header_len = ntohl(*(uint32_t*)(header_buf + 4));
  if (total_len < 4 || total_len > kMaxFrameSize) return false;
  if (header_len > total_len - 4) return false;

  // 读 Header + Body
  uint32_t remaining = total_len - 4;
  std::vector<char> frame(remaining);
  received = 0;
  while (received < remaining) {
    ssize_t n = recv(fd, frame.data() + received, remaining - received, 0);
    if (n == 0) return false;
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return true;
      return false;
    }
    received += n;
  }

  // 解析
  xrpc::RpcHeader rpc_header;
  if (!rpc_header.ParseFromArray(frame.data(), header_len)) return false;

  uint32_t body_len = remaining - header_len;
  Kuser::LoginRequest login_req;
  if (!login_req.ParseFromArray(frame.data() + header_len, body_len)) return false;

  // 业务处理
  Kuser::LoginResponse login_resp;
  login_resp.mutable_result()->set_errcode(0);
  login_resp.mutable_result()->set_errmsg("ok");
  login_resp.set_success(true);

  // 序列化
  std::string resp_body;
  if (!login_resp.SerializeToString(&resp_body)) return false;

  // 发送 [4B BodyLen(BE)][Body]
  uint32_t resp_len = htonl(static_cast<uint32_t>(resp_body.size()));
  struct iovec iov[2];
  iov[0].iov_base = &resp_len;
  iov[0].iov_len = 4;
  iov[1].iov_base = (void*)resp_body.data();
  iov[1].iov_len = resp_body.size();

  size_t to_send = 4 + resp_body.size();
  size_t sent = 0;
  while (sent < to_send) {
    ssize_t n = writev(fd, iov, 2);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::yield();
        continue;
      }
      return false;
    }
    sent += n;
    // 调整 iov 偏移
    while (n > 0) {
      if ((size_t)n >= iov[0].iov_len) {
        n -= iov[0].iov_len;
        iov[0].iov_len = 0;
        if (n > 0) {
          iov[1].iov_base = (char*)iov[1].iov_base + n;
          iov[1].iov_len -= n;
          n = 0;
        }
      } else {
        iov[0].iov_base = (char*)iov[0].iov_base + n;
        iov[0].iov_len -= n;
        n = 0;
      }
    }
  }
  return true;
}

// ============================================================================
// Worker EventLoop
// ============================================================================
struct WorkerContext {
  std::mutex mtx;
  std::vector<int> new_fds;          // 主线程写入，worker 线程读取
  std::atomic<long long>* counter;   // 全局请求计数器
};

void WorkerLoop(int worker_id, WorkerContext* ctx) {
  struct pollfd fds[kMaxFds];
  int num_fds = 0;

  while (g_running.load(std::memory_order_relaxed)) {
    // 1. 检查新 fd
    {
      std::lock_guard<std::mutex> lk(ctx->mtx);
      for (int fd : ctx->new_fds) {
        if (num_fds < kMaxFds) {
          fds[num_fds].fd = fd;
          fds[num_fds].events = POLLIN;
          fds[num_fds].revents = 0;
          num_fds++;
        } else {
          close(fd);  // 容量满
        }
      }
      ctx->new_fds.clear();
    }

    // 2. poll() — 如果没有任何 fd，用短超时等待新连接
    int timeout = (num_fds > 0) ? 1 : 10;  // ms
    int ready = poll(fds, num_fds, timeout);

    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) continue;

    // 3. 处理就绪 fd
    for (int i = 0; i < num_fds && ready > 0; ++i) {
      if (fds[i].revents == 0) continue;
      ready--;

      if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        close(fds[i].fd);
        if (i < num_fds - 1) {
          fds[i] = fds[num_fds - 1];
        }
        num_fds--;
        i--;
        continue;
      }

      if (fds[i].revents & POLLIN) {
        bool ok = ProcessOneRequest(fds[i].fd);
        if (ok) {
          ctx->counter->fetch_add(1, std::memory_order_relaxed);
        } else {
          close(fds[i].fd);
          if (i < num_fds - 1) {
            fds[i] = fds[num_fds - 1];
          }
          num_fds--;
          i--;
        }
      }
    }
  }

  // 清理
  for (int i = 0; i < num_fds; ++i) close(fds[i].fd);
}

// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
  int port = 9000;
  int num_workers = 4;
  if (argc > 1) port = std::atoi(argv[1]);
  if (argc > 2) num_workers = std::atoi(argv[2]);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, [](int) { g_running = false; });

  // ---- 监听 socket ----
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) { perror("socket"); return 1; }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
  SetNonBlocking(listen_fd);

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind"); return 1;
  }
  if (listen(listen_fd, 65535) < 0) {
    perror("listen"); return 1;
  }

  std::cout << "[Reactor] port=" << port << " workers=" << num_workers
            << " (poll multiplexing)\n";

  // ---- 启动 Worker ----
  std::atomic<long long> total_reqs(0);
  std::vector<WorkerContext> contexts(num_workers);
  std::vector<std::thread> worker_threads;

  for (int i = 0; i < num_workers; ++i) {
    contexts[i].counter = &total_reqs;
    worker_threads.emplace_back(WorkerLoop, i, &contexts[i]);
  }

  // ---- Accept Loop (Main Thread) ----
  int rr_idx = 0;
  while (g_running.load(std::memory_order_relaxed)) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);

    if (client_fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        poll(&pfd, 1, 100);
        continue;
      }
      break;
    }

    SetNonBlocking(client_fd);
    // 禁用 Nagle 算法，减少小包延迟
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // RoundRobin 分发
    int idx = rr_idx % num_workers;
    rr_idx++;
    {
      std::lock_guard<std::mutex> lk(contexts[idx].mtx);
      contexts[idx].new_fds.push_back(client_fd);
    }
  }

  g_running.store(false);
  close(listen_fd);

  for (auto& t : worker_threads) t.join();

  std::cout << "[Reactor] done. requests=" << total_reqs.load() << "\n";
  return 0;
}
