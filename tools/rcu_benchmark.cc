// ============================================================================
// RCU vs RwLock vs Mutex — 路由表查表性能对比 V2
// ============================================================================
// 模拟真实 RPC 场景：
//   8 读线程持续高频查表，每轮 1000 次读后记录一次延迟样本
//   写线程按不同压力等级触发：低(1/s) / 中(100/s) / 高(1000/s)
// 核心指标：写操作期间读延迟的 P50/P99/P999 抖动
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using RouteTable = std::unordered_map<std::string, std::vector<std::string>>;
using Clock = std::chrono::high_resolution_clock;

// ---- 数据准备 ----
std::shared_ptr<const RouteTable> MakeTestTable() {
  auto t = std::make_shared<RouteTable>();
  for (int i = 0; i < 100; ++i) {
    std::string path = "/Service" + std::to_string(i) + "/Method";
    std::vector<std::string> nodes;
    for (int j = 0; j < 10; ++j)
      nodes.push_back("10.0.0." + std::to_string(j) + ":8000");
    (*t)[path] = nodes;
  }
  return t;
}

std::vector<std::string> g_query_paths;
void InitPaths() {
  for (int i = 0; i < 100; ++i)
    g_query_paths.push_back("/Service" + std::to_string(i) + "/Method");
}

// ---- 分位数计算 ----
struct LatencyStats {
  double p50_ns, p99_ns, p999_ns, avg_ns, max_ns;
  long long count;
};
LatencyStats ComputePercentiles(std::vector<long long>& samples) {
  LatencyStats s{};
  if (samples.empty()) return s;
  std::sort(samples.begin(), samples.end());
  s.count = samples.size();
  long long sum = 0;
  for (auto v : samples) sum += v;
  s.avg_ns = (double)sum / s.count;
  s.max_ns = samples.back();
  s.p50_ns = samples[s.count * 50 / 100];
  s.p99_ns = samples[s.count * 99 / 100];
  s.p999_ns = samples[s.count * 999 / 1000];
  return s;
}

// ============================================================================
// 方案 A: RCU (与 Xrpc 实际实现一致)
// ============================================================================
struct RCUBench {
  std::shared_ptr<const RouteTable> table;
  std::mutex write_mutex;
  RCUBench() : table(MakeTestTable()) {}

  std::vector<std::string> Read(const std::string& path) {
    auto local = std::atomic_load(&table);
    auto it = local->find(path);
    if (it != local->end()) return it->second;
    return {};
  }
  void Write(const std::string& path) {
    std::lock_guard<std::mutex> lk(write_mutex);
    auto old = std::atomic_load(&table);
    auto n = std::make_shared<RouteTable>(*old);
    (*n)[path] = {"10.0.0.1:8000", "10.0.0.2:8000"};
    std::atomic_store(&table, std::shared_ptr<const RouteTable>(n));
  }
};

// ============================================================================
// 方案 B: std::shared_mutex
// ============================================================================
struct RwLockBench {
  std::shared_ptr<const RouteTable> table;
  mutable std::shared_mutex rwlock;
  RwLockBench() : table(MakeTestTable()) {}

  std::vector<std::string> Read(const std::string& path) {
    std::shared_lock<std::shared_mutex> lk(rwlock);
    auto local = table;  // shared_ptr 拷贝引用计数
    lk.unlock();
    auto it = local->find(path);
    if (it != local->end()) return it->second;
    return {};
  }
  void Write(const std::string& path) {
    std::unique_lock<std::shared_mutex> lk(rwlock);
    auto n = std::make_shared<RouteTable>(*table);
    (*n)[path] = {"10.0.0.1:8000", "10.0.0.2:8000"};
    table = std::shared_ptr<const RouteTable>(n);
  }
};

// ============================================================================
// 方案 C: std::mutex
// ============================================================================
struct MutexBench {
  std::shared_ptr<const RouteTable> table;
  mutable std::mutex mtx;
  MutexBench() : table(MakeTestTable()) {}

  std::vector<std::string> Read(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx);
    auto local = table;
    auto it = local->find(path);
    if (it != local->end()) return it->second;
    return {};
  }
  void Write(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx);
    auto n = std::make_shared<RouteTable>(*table);
    (*n)[path] = {"10.0.0.1:8000", "10.0.0.2:8000"};
    table = std::shared_ptr<const RouteTable>(n);
  }
};

// ---- 通用 benchmark: 采集每次读操作的延迟样本 ----
template <typename T>
void RunLatencyBench(T& bench, int readers, int duration_sec, int writes_per_sec,
                     std::vector<long long>& latency_ns) {
  std::atomic<bool> running(true);
  std::mutex samples_mutex;

  // 写线程
  std::thread writer([&]() {
    int idx = 0;
    while (running.load(std::memory_order_relaxed)) {
      auto t0 = Clock::now();
      bench.Write(g_query_paths[idx % 100]);
      idx++;
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                         Clock::now() - t0)
                         .count();
      int sleep_us =
          std::max(0, static_cast<int>(1'000'000 / writes_per_sec - elapsed));
      if (sleep_us > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
  });

  // 读线程
  std::vector<std::thread> rdrs;
  for (int t = 0; t < readers; ++t) {
    rdrs.emplace_back([&, t]() {
      int pi = t * 13;
      while (running.load(std::memory_order_relaxed)) {
        auto t0 = Clock::now();
        bench.Read(g_query_paths[pi % 100]);
        auto t1 = Clock::now();
        long long ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count();
        pi++;
        // 每 1000 次记录一个样本，避免样本量过大
        // 用 local counter 减少 atomic 开销
        static thread_local int sample_counter = 0;
        if (++sample_counter % 1000 == 0) {
          std::lock_guard<std::mutex> lk(samples_mutex);
          latency_ns.push_back(ns);
        }
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
  running.store(false);
  writer.join();
  for (auto& r : rdrs) r.join();
}

// ---- 打印 ----
void PrintSection(const std::string& title, const LatencyStats& s) {
  auto ns2us = [](double ns) -> double { return ns / 1000.0; };
  std::cout << "  " << title << " (" << s.count << " samples):\n"
            << "    avg=" << std::fixed << std::setprecision(1) << ns2us(s.avg_ns)
            << "us  P50=" << ns2us(s.p50_ns)
            << "us  P99=" << ns2us(s.p99_ns)
            << "us  P999=" << ns2us(s.p999_ns)
            << "us  max=" << ns2us(s.max_ns) << "us\n";
  double tail_factor = s.p50_ns > 0 ? s.p999_ns / s.p50_ns : 0;
  std::cout << "    tail-ratio(P999/P50)=" << std::fixed << std::setprecision(1)
            << tail_factor << "x";
  if (tail_factor > 10)
    std::cout << " ⚠️  严重长尾";
  else if (tail_factor > 3)
    std::cout << " ⚡ 中等长尾";
  else
    std::cout << " ✓ 健康";
  std::cout << "\n\n";
}

int main() {
  InitPaths();

  const int kReaders = 8;
  const int kDur = 5;

  struct Config {
    int wps;
    const char* label;
  };
  Config configs[] = {
      {500, "高写压力 (500/s)"},
      {100, "中写压力 (100/s)"},
      {1, "低写压力 (1/s)"},
  };

  std::cout << "\n╔══════════════════════════════════════════════════════════════════╗\n"
            << "║  RCU vs RwLock vs Mutex — 读延迟分析（写操作对读的长尾影响）    ║\n"
            << "║  读线程: " << kReaders << "  测试时长: " << kDur
            << "s/轮  采样间隔: 1/1000               ║\n"
            << "╚══════════════════════════════════════════════════════════════════╝\n\n";

  for (const auto& cfg : configs) {
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << "  " << cfg.label << "\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<long long> rcu_lat, rwl_lat, mtx_lat;

    {
      std::cout << "  [RCU]  测量中..." << std::flush;
      RCUBench rcu;
      RunLatencyBench(rcu, kReaders, kDur, cfg.wps, rcu_lat);
      std::cout << " 完成\n";
    }
    {
      std::cout << "  [RwLock] 测量中..." << std::flush;
      RwLockBench rwl;
      RunLatencyBench(rwl, kReaders, kDur, cfg.wps, rwl_lat);
      std::cout << " 完成\n";
    }
    {
      std::cout << "  [Mutex] 测量中..." << std::flush;
      MutexBench mtx;
      RunLatencyBench(mtx, kReaders, kDur, cfg.wps, mtx_lat);
      std::cout << " 完成\n\n";
    }

    auto srcu = ComputePercentiles(rcu_lat);
    auto srwl = ComputePercentiles(rwl_lat);
    auto smtx = ComputePercentiles(mtx_lat);

    PrintSection("RCU", srcu);
    PrintSection("RwLock", srwl);
    PrintSection("Mutex", smtx);

    // 对比表
    auto us = [](double ns) { return ns / 1000.0; };
    std::cout << "  ╔══════════╦═══════════╦═══════════╦═══════════╦══════════╗\n"
              << "  ║          ║    P50    ║    P99    ║   P999    ║ P999/P50 ║\n"
              << "  ╠══════════╬═══════════╬═══════════╬═══════════╬══════════╣\n";
    std::cout << "  ║ RCU      ║ " << std::setw(7) << std::fixed
              << std::setprecision(1) << us(srcu.p50_ns) << "us ║ "
              << std::setw(7) << us(srcu.p99_ns) << "us ║ "
              << std::setw(7) << us(srcu.p999_ns) << "us ║ "
              << std::setw(6) << (srcu.p50_ns > 0 ? srcu.p999_ns / srcu.p50_ns : 0)
              << "x ║\n";
    std::cout << "  ║ RwLock   ║ " << std::setw(7) << us(srwl.p50_ns)
              << "us ║ " << std::setw(7) << us(srwl.p99_ns) << "us ║ "
              << std::setw(7) << us(srwl.p999_ns) << "us ║ "
              << std::setw(6) << (srwl.p50_ns > 0 ? srwl.p999_ns / srwl.p50_ns : 0)
              << "x ║\n";
    std::cout << "  ║ Mutex    ║ " << std::setw(7) << us(smtx.p50_ns)
              << "us ║ " << std::setw(7) << us(smtx.p99_ns) << "us ║ "
              << std::setw(7) << us(smtx.p999_ns) << "us ║ "
              << std::setw(6) << (smtx.p50_ns > 0 ? smtx.p999_ns / smtx.p50_ns : 0)
              << "x ║\n";
    std::cout << "  ╚══════════╩═══════════╩═══════════╩═══════════╩══════════╝\n\n";

    // 结论
    std::cout << "  💡 分析: ";
    double rcu_ratio = srcu.p999_ns / std::max(srcu.p50_ns, 1.0);
    double rwl_ratio = srwl.p999_ns / std::max(srwl.p50_ns, 1.0);
    double mtx_ratio = smtx.p999_ns / std::max(smtx.p50_ns, 1.0);

    if (rcu_ratio <= rwl_ratio && rcu_ratio <= mtx_ratio) {
      std::cout << "RCU 长尾控制最好 (P999/P50=" << rcu_ratio << "x)\n\n";
    } else if (rwl_ratio <= rcu_ratio && rwl_ratio <= mtx_ratio) {
      std::cout << "RwLock 长尾控制最好 (P999/P50=" << rwl_ratio << "x)\n\n";
    } else {
      std::cout << "Mutex 长尾控制最好 (P999/P50=" << mtx_ratio << "x)\n\n";
    }
  }

  std::cout << "════════════════════════════════════════════════════════════\n"
            << "注意: macOS libc++ 上 std::atomic_load<shared_ptr> 使用了\n"
            << "全局 spinlock，导致 RCU 读路径仍有内部锁竞争。在 Linux\n"
            << "libstdc++ 上该操作是 lock-free 的，RCU 优势会更加明显。\n"
            << "════════════════════════════════════════════════════════════\n\n";

  return 0;
}
