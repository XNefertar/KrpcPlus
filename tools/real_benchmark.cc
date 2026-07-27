// ============================================================================
// Xrpc 真实全链路压测 — 不依赖框架，纯 POSIX socket + Protobuf
// ============================================================================
// 对接 mini_server (Legacy 协议)，测量真实 TCP 链路的 QPS / 延迟分位数。
//
// Legacy 请求格式: [4B TotalLen(BE)][4B HeaderLen(BE)][RpcHeader][Body]
// Legacy 响应格式: [4B TotalLen(BE)][Body]
//
// 编译 (在 Xrpc 项目根目录):
//   g++ -std=c++17 -O2 -pthread \
//       tools/real_benchmark.cc \
//       example/user.pb.cc src/protocol/rpc_header.pb.cc \
//       -I . -I src/include \
//       $(pkg-config --cflags --libs protobuf) \
//       -o /tmp/real_benchmark
//
// 运行:
//   1. 先启动 mini_server:    ./tools/mini_server 9000
//   2. 运行压测:              /tmp/real_benchmark 127.0.0.1 9000 50 2000
// ============================================================================

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "example/user.pb.h"
#include "src/protocol/rpc_header.pb.h"

// ---- 计时 ----
using Clock = std::chrono::high_resolution_clock;

// ---- 精确读 N 字节 (处理 TCP 拆包) ----
ssize_t recv_exact(int fd, char* buf, size_t size) {
  size_t total = 0;
  while (total < size) {
    ssize_t n = recv(fd, buf + total, size - total, 0);
    if (n <= 0) return n;
    total += n;
  }
  return total;
}

// ---- 分位数计算 ----
struct LatencyStats {
  double p50_ms, p99_ms, p999_ms, avg_ms, max_ms;
  long long count;
};
LatencyStats ComputePercentiles(std::vector<double>& samples) {
  LatencyStats s{};
  if (samples.empty()) return s;
  std::sort(samples.begin(), samples.end());
  s.count = samples.size();
  double sum = 0;
  for (auto v : samples) sum += v;
  s.avg_ms = sum / s.count;
  s.max_ms = samples.back();
  s.p50_ms = samples[s.count * 50 / 100];
  s.p99_ms = samples[s.count * 99 / 100];
  s.p999_ms = samples[s.count * 999 / 1000];
  return s;
}

// ---- 一次 RPC 调用 (阻塞) ----
bool RpcCall(int fd, const std::string& service, const std::string& method,
             const std::string& req_body, std::string& resp_body,
             double& latency_ms) {
  auto t0 = Clock::now();

  // === 1. 编码请求 ===
  // RpcHeader
  xrpc::RpcHeader header;
  header.set_service_name(service);
  header.set_method_name(method);
  header.set_args_size(req_body.size());
  std::string header_bytes;
  if (!header.SerializeToString(&header_bytes)) return false;

  // 构造 wire: [4B TotalLen(BE)][4B HeaderLen(BE)][Header][Body]
  uint32_t header_len = header_bytes.size();
  uint32_t total_len = 4 + header_len + req_body.size();
  std::vector<char> wire(8 + header_len + req_body.size());
  uint32_t net_total = htonl(total_len);
  uint32_t net_header = htonl(header_len);
  std::memcpy(wire.data(), &net_total, 4);
  std::memcpy(wire.data() + 4, &net_header, 4);
  std::memcpy(wire.data() + 8, header_bytes.data(), header_len);
  if (!req_body.empty())
    std::memcpy(wire.data() + 8 + header_len, req_body.data(), req_body.size());

  // === 2. 发送 ===
  if (send(fd, wire.data(), wire.size(), 0) != (ssize_t)wire.size()) {
    return false;
  }

  // === 3. 接收响应: [4B TotalLen(BE)][Body] ===
  uint32_t resp_len_be = 0;
  if (recv_exact(fd, (char*)&resp_len_be, 4) != 4) return false;
  uint32_t resp_len = ntohl(resp_len_be);
  if (resp_len > 16 * 1024 * 1024) return false;

  std::vector<char> body_buf(resp_len);
  if (resp_len > 0) {
    if (recv_exact(fd, body_buf.data(), resp_len) != (ssize_t)resp_len)
      return false;
  }

  // === 4. 反序列化响应 ===
  Kuser::LoginResponse resp;
  if (!resp.ParseFromArray(body_buf.data(), resp_len)) return false;

  auto t1 = Clock::now();
  latency_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  resp_body = resp.result().errmsg();
  return resp.result().errcode() == 0;
}

// ---- Worker 线程 ----
void Worker(int thread_id, const std::string& ip, int port,
            int calls_per_thread, std::atomic<long long>& success,
            std::atomic<long long>& fail,
            std::vector<double>& all_latencies, std::mutex& lat_mutex) {
  // 建立 TCP 连接
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fail.fetch_add(calls_per_thread);
    return;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

  // 连接重试 (高并发下 accept 队列可能打满)
  int retry = 0;
  while (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    if (++retry >= 20) {
      close(fd);
      fail.fetch_add(calls_per_thread);
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100 * retry));
  }

  // 预序列化 LoginRequest (所有请求共用)
  Kuser::LoginRequest req;
  req.set_name("zhangsan");
  req.set_pwd("123456");
  std::string req_body;
  if (!req.SerializeToString(&req_body)) {
    close(fd);
    fail.fetch_add(calls_per_thread);
    return;
  }

  // 执行 RPC 调用
  for (int i = 0; i < calls_per_thread; ++i) {
    std::string resp_body;
    double lat_ms = 0;

    if (RpcCall(fd, "UserServiceRpc", "Login", req_body, resp_body, lat_ms)) {
      success.fetch_add(1, std::memory_order_relaxed);

      // 记录延迟样本 (每 10 次记录一个，避免样本量过大)
      if (i % 10 == 0) {
        std::lock_guard<std::mutex> lk(lat_mutex);
        all_latencies.push_back(lat_ms);
      }
    } else {
      fail.fetch_add(1, std::memory_order_relaxed);
    }
  }

  close(fd);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0]
              << " <ip> <port> [threads=20] [calls_per_thread=500]\n";
    return 1;
  }

  std::string ip = argv[1];
  int port = std::atoi(argv[2]);
  int threads = (argc > 3) ? std::atoi(argv[3]) : 20;
  int calls_per = (argc > 4) ? std::atoi(argv[4]) : 500;
  long long total = (long long)threads * calls_per;

  std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
            << "║   XRPC Real-Link Benchmark (Raw TCP + Protobuf)      ║\n"
            << "╠══════════════════════════════════════════════════════╣\n"
            << "║  Target: " << ip << ":" << port << "\n"
            << "║  Threads: " << threads
            << "  Calls/T: " << calls_per
            << "  Total: " << total << "\n"
            << "╚══════════════════════════════════════════════════════╝\n\n";

  // ---- 启动 ----
  std::atomic<long long> success(0), fail(0);
  std::vector<double> latencies;
  std::mutex lat_mutex;

  auto t0 = Clock::now();

  std::vector<std::thread> workers;
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back(Worker, i, std::ref(ip), port, calls_per,
                         std::ref(success), std::ref(fail), std::ref(latencies),
                         std::ref(lat_mutex));
  }

  for (auto& t : workers) t.join();

  auto t1 = Clock::now();
  double elapsed =
      std::chrono::duration<double, std::milli>(t1 - t0).count() / 1000.0;

  long long s = success.load();
  long long f = fail.load();
  long long t = s + f;

  // ---- 统计 ----
  double qps = t / elapsed;
  double succ_rate = t > 0 ? 100.0 * s / t : 0;

  auto stats = ComputePercentiles(latencies);

  // ---- 报告 ----
  std::cout << "╔══════════════════════════════════════════════════════╗\n"
            << "║           Real-Link Benchmark — Final Report          ║\n"
            << "╠══════════════════════════════════════════════════════╣\n";

  std::cout << "║ [Basic]\n"
            << "║   Total:     " << std::setw(10) << t << "\n"
            << "║   Success:   " << std::setw(10) << s << "\n"
            << "║   Fail:      " << std::setw(10) << f << "\n"
            << "║   Elapsed:   "
            << std::setw(7) << std::fixed << std::setprecision(3) << elapsed
            << " s\n"
            << "║   Success Rate: " << std::setw(5) << std::setprecision(2)
            << succ_rate << " %\n"
            << "╠══════════════════════════════════════════════════════╣\n";

  std::cout << "║ [Throughput]\n"
            << "║   Overall QPS:  " << std::setw(10) << std::setprecision(1)
            << qps << " req/s\n"
            << "╠══════════════════════════════════════════════════════╣\n";

  std::cout << "║ [Latency]  (samples: " << stats.count << ")\n"
            << "║   Avg:  " << std::setw(8) << std::setprecision(3) << stats.avg_ms
            << " ms\n"
            << "║   P50:  " << std::setw(8) << std::setprecision(3) << stats.p50_ms
            << " ms\n"
            << "║   P99:  " << std::setw(8) << std::setprecision(3) << stats.p99_ms
            << " ms\n"
            << "║   P999: " << std::setw(8) << std::setprecision(3) << stats.p999_ms
            << " ms\n"
            << "║   Max:  " << std::setw(8) << std::setprecision(3) << stats.max_ms
            << " ms\n";

  double ratio = stats.p50_ms > 0.001 ? stats.p999_ms / stats.p50_ms : 0;
  std::cout << "║   P999/P50: " << std::setw(6) << std::setprecision(1) << ratio
            << "x";
  if (ratio > 10)
    std::cout << " ⚠️ 严重长尾";
  else if (ratio > 3)
    std::cout << " ⚡ 中等长尾";
  else
    std::cout << " ✓ 健康";
  std::cout << "\n";

  std::cout << "╚══════════════════════════════════════════════════════╝\n\n";

  // 简单结论
  std::cout << "结论: 真实 TCP 链路上 (" << ip << ":" << port << ")\n"
            << "  单连接短请求模式下 " << threads << " 并发线程实现 "
            << std::setprecision(0) << qps << " QPS，\n"
            << "  端到端延迟 P50=" << std::setprecision(3) << stats.p50_ms
            << "ms, P99=" << stats.p99_ms
            << "ms, P999=" << stats.p999_ms << "ms。\n"
            << "  链路包含: TCP建连 + Protobuf序列化 + send/recv系统调用 + "
               "服务端处理 + Protobuf反序列化。\n\n";

  return 0;
}
