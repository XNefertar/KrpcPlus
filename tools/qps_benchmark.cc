// ============================================================================
// QPS Benchmark Tool — 独立压测工具
// ============================================================================
// 模拟多线程 RPC 调用场景，输出完整的 QPS 分析报告。
// 零外部依赖，纯 C++17 标准库。
//
// 编译: g++ -std=c++17 -O2 tools/qps_benchmark.cc -o tools/qps_benchmark -lpthread
// 运行: ./tools/qps_benchmark
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

// ============================================================================
// 耗时分布模拟器 — 模拟真实 RPC 调用的各阶段耗时
// ============================================================================
class LatencySimulator {
 public:
  explicit LatencySimulator(uint64_t seed = 42) : gen_(seed) {}

  // 模拟一次完整 RPC 调用 (含 9 阶段)，返回总耗时(ms)
  double SimulateCall() {
    double total = 0.0;

    // Stage 1: ZK 服务发现 (首次 ~2ms, 缓存命中 ~0.001ms)
    total += zk_hit_ ? std::abs(cache_dist_(gen_)) : std::abs(zk_dist_(gen_));

    // Stage 2: 负载均衡 (~0.001ms)
    total += std::abs(lb_dist_(gen_));

    // Stage 3: TCP 建连 (仅首次 ~0.5ms)
    if (!zk_hit_) total += std::abs(conn_dist_(gen_));

    // Stage 4: 序列化 (~0.015ms)
    total += std::abs(ser_dist_(gen_));

    // Stage 5: 协议编码 (~0.005ms)
    total += std::abs(enc_dist_(gen_));

    // Stage 6: 网络 IO (~0.2ms, 3% 概率触发 5-20ms 长尾)
    total += std::abs(net_dist_(gen_));

    // Stage 7: 协议解码 (~0.005ms)
    total += std::abs(dec_dist_(gen_));

    // Stage 8: 反序列化 (~0.012ms)
    total += std::abs(des_dist_(gen_));

    // 3% 概率触发长尾 (模拟网络抖动 / GC 暂停)
    if (tail_trigger_(gen_) < 0.03) {
      total += tail_dist_(gen_);
    }

    zk_hit_ = true;
    return total;
  }

 private:
  std::mt19937_64 gen_;
  bool zk_hit_ = false;

  std::normal_distribution<> zk_dist_{2.0, 0.8};
  std::normal_distribution<> cache_dist_{0.001, 0.0008};
  std::normal_distribution<> lb_dist_{0.001, 0.0005};
  std::normal_distribution<> conn_dist_{0.5, 0.3};
  std::normal_distribution<> ser_dist_{0.015, 0.008};
  std::normal_distribution<> enc_dist_{0.005, 0.003};
  std::normal_distribution<> net_dist_{0.2, 0.15};
  std::normal_distribution<> dec_dist_{0.005, 0.003};
  std::normal_distribution<> des_dist_{0.012, 0.006};

  std::uniform_real_distribution<> tail_trigger_{0.0, 1.0};
  std::uniform_real_distribution<> tail_dist_{5.0, 20.0};
};

// ============================================================================
// 计时辅助
// ============================================================================
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

inline double ElapsedMs(TimePoint start, TimePoint end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

// ============================================================================
// Worker 线程 — 执行压测请求
// ============================================================================
void WorkerThread(int thread_id, int calls_per_thread,
                  std::atomic<long long>& success_count,
                  std::atomic<long long>& fail_count,
                  std::atomic<long long>& total_latency_us) {
  LatencySimulator sim(42 + thread_id * 10007);
  std::mt19937_64 fail_gen(thread_id * 131);

  for (int i = 0; i < calls_per_thread; ++i) {
    double latency_ms = sim.SimulateCall();

    // 实际操作: sleep 模拟网络等待
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(latency_ms * 1'000'000)));

    // 模拟 0.5% 的失败率 (实际 RPC 场景)
    if (std::uniform_real_distribution<>(0, 1)(fail_gen) < 0.005) {
      fail_count.fetch_add(1, std::memory_order_relaxed);
    } else {
      success_count.fetch_add(1, std::memory_order_relaxed);
      total_latency_us.fetch_add(
          static_cast<long long>(latency_ms * 1000), std::memory_order_relaxed);
    }
  }
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
  // ---- 参数解析 ----
  int thread_count = 100;
  int calls_per_thread = 10000;
  int monitor_interval_ms = 1000;  // 监控采样间隔

  if (argc > 1) thread_count = std::atoi(argv[1]);
  if (argc > 2) calls_per_thread = std::atoi(argv[2]);
  if (argc > 3) monitor_interval_ms = std::atoi(argv[3]);

  const long long total_requests =
      static_cast<long long>(thread_count) * calls_per_thread;

  std::cout << "\n"
            << "╔══════════════════════════════════════════════════╗\n"
            << "║       XRPC QPS Benchmark Tool v2.0              ║\n"
            << "╠══════════════════════════════════════════════════╣\n"
            << "║  Threads:       " << std::setw(8) << thread_count << "                       ║\n"
            << "║  Calls/Thread:  " << std::setw(8) << calls_per_thread << "                       ║\n"
            << "║  Total Calls:   " << std::setw(8) << total_requests << "                       ║\n"
            << "║  Monitor Intvl: " << std::setw(8) << monitor_interval_ms << "ms                     ║\n"
            << "╚══════════════════════════════════════════════════╝\n\n";

  // ---- 共享状态 ----
  std::atomic<long long> success_count(0);
  std::atomic<long long> fail_count(0);
  std::atomic<long long> total_latency_us(0);
  std::atomic<bool> running(true);

  // ---- 历史数据 (监控线程写入, 主线程读取, 中间无并发) ----
  std::vector<double> qps_history;
  std::vector<double> latency_history;
  std::vector<double> success_rate_history;

  auto global_start = Clock::now();

  // ================================================================
  // 实时 QPS 监控线程
  // ================================================================
  std::thread monitor([&]() {
    auto prev_time = global_start;
    long long prev_success = 0;
    long long prev_fail = 0;
    int tick = 0;

    std::cout << "┌────────┬────────────┬────────────┬────────────┬────────────┬──────────────┐\n"
              << "│  Time  │  Interval  │ Cumul. Avg │  Success   │    Fail    │ Success Rate │\n"
              << "│   (s)  │    QPS     │    QPS     │    QPS     │    QPS     │              │\n"
              << "├────────┼────────────┼────────────┼────────────┼────────────┼──────────────┤\n";

    while (running.load(std::memory_order_relaxed)) {
      auto sleep_ms = monitor_interval_ms;
      auto sleep_start = Clock::now();
      while (std::chrono::duration<double, std::milli>(Clock::now() - sleep_start).count() < sleep_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!running.load(std::memory_order_relaxed)) break;
      }
      if (!running.load(std::memory_order_relaxed)) break;

      auto now = Clock::now();
      std::chrono::duration<double> from_start = now - global_start;
      std::chrono::duration<double> interval = now - prev_time;

      long long cur_success = success_count.load(std::memory_order_relaxed);
      long long cur_fail = fail_count.load(std::memory_order_relaxed);
      long long cur_total = cur_success + cur_fail;

      if (interval.count() < 0.001) continue;

      long long delta_success = cur_success - prev_success;
      long long delta_fail = cur_fail - prev_fail;
      long long delta_total = delta_success + delta_fail;

      double interval_qps = delta_total / interval.count();
      double cumulative_qps =
          cur_total / std::max(0.001, from_start.count());
      double succ_qps = delta_success / interval.count();
      double fail_qps = delta_fail / interval.count();
      double rate =
          delta_total > 0 ? (100.0 * delta_success / delta_total) : 100.0;

      if (delta_total > 0) {
        qps_history.push_back(interval_qps);
        success_rate_history.push_back(rate);
        // 平均延迟从累计中计算
        long long lat_us = total_latency_us.load(std::memory_order_relaxed);
        double avg_lat = cur_success > 0 ? (lat_us / 1000.0 / cur_success) : 0.0;
        latency_history.push_back(avg_lat);
      }

      tick++;
      int elapsed_sec = static_cast<int>(from_start.count());

      std::cout << "│ " << std::setw(6) << elapsed_sec
                << " │ " << std::setw(10) << std::fixed << std::setprecision(1)
                << interval_qps
                << " │ " << std::setw(10) << std::fixed << std::setprecision(1)
                << cumulative_qps
                << " │ " << std::setw(10) << std::fixed << std::setprecision(1)
                << succ_qps
                << " │ " << std::setw(10) << std::fixed << std::setprecision(1)
                << fail_qps
                << " │ " << std::setw(5) << std::fixed << std::setprecision(1)
                << rate << "%       │\n"
                << std::flush;

      prev_time = now;
      prev_success = cur_success;
      prev_fail = cur_fail;
    }
  });

  // ================================================================
  // 启动 Worker 线程
  // ================================================================
  std::vector<std::thread> workers;
  for (int i = 0; i < thread_count; ++i) {
    workers.emplace_back(WorkerThread, i, calls_per_thread,
                         std::ref(success_count), std::ref(fail_count),
                         std::ref(total_latency_us));
  }

  // 等待所有 Worker 完成
  for (auto& t : workers) t.join();

  // 停止监控
  running.store(false, std::memory_order_relaxed);
  auto global_end = Clock::now();
  monitor.join();

  std::chrono::duration<double> elapsed = global_end - global_start;

  long long final_success = success_count.load();
  long long final_fail = fail_count.load();
  long long final_total = final_success + final_fail;

  // ================================================================
  // QPS 统计分析
  // ================================================================
  double overall_qps = final_total / elapsed.count();
  double success_qps = final_success / elapsed.count();
  double success_rate = final_total > 0 ? (100.0 * final_success / final_total) : 0.0;

  double peak_qps = 0.0, min_qps = 1e18;
  double qps_sum = 0.0;
  for (double q : qps_history) {
    peak_qps = std::max(peak_qps, q);
    min_qps = std::min(min_qps, q);
    qps_sum += q;
  }
  int samples = static_cast<int>(qps_history.size());
  double avg_interval_qps = samples > 0 ? qps_sum / samples : 0.0;

  // 标准差
  double qps_variance = 0.0;
  for (double q : qps_history) {
    qps_variance += (q - avg_interval_qps) * (q - avg_interval_qps);
  }
  if (samples > 1) qps_variance /= (samples - 1);  // 样本标准差
  double qps_stddev = std::sqrt(qps_variance);
  double stability = avg_interval_qps > 0 ? (1.0 - qps_stddev / avg_interval_qps) * 100.0 : 0.0;

  // 延迟统计
  double avg_latency_ms = final_success > 0
      ? (total_latency_us.load() / 1000.0 / final_success) : 0.0;

  // ================================================================
  // 最终报告
  // ================================================================
  std::cout << "└────────┴────────────┴────────────┴────────────┴────────────┴──────────────┘\n\n";

  std::cout << "╔══════════════════════════════════════════════════════════════════╗\n"
            << "║               QPS Benchmark — Final Report                      ║\n"
            << "╠══════════════════════════════════════════════════════════════════╣\n";

  std::cout << "║  [Basic Metrics]\n";
  std::cout << "║    Total Requests:   " << std::setw(12) << total_requests << "                    ║\n";
  std::cout << "║    Completed:        " << std::setw(12) << final_total << "                    ║\n";
  std::cout << "║    Success:          " << std::setw(12) << final_success << "                    ║\n";
  std::cout << "║    Fail:             " << std::setw(12) << final_fail << "                    ║\n";
  std::cout << "║    Elapsed:          " << std::setw(9) << std::fixed << std::setprecision(3)
            << elapsed.count() << " s                   ║\n";
  std::cout << "║    Success Rate:     " << std::setw(9) << std::fixed << std::setprecision(2)
            << success_rate << " %                  ║\n";
  std::cout << "║    Avg Latency:      " << std::setw(9) << std::fixed << std::setprecision(3)
            << avg_latency_ms << " ms                 ║\n";
  std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";

  std::cout << "║  [QPS Metrics]\n";
  std::cout << "║    Overall QPS:      " << std::setw(12) << std::fixed << std::setprecision(1)
            << overall_qps << "  req/s            ║\n";
  std::cout << "║    Success QPS:      " << std::setw(12) << std::fixed << std::setprecision(1)
            << success_qps << "  req/s            ║\n";
  std::cout << "║    Peak QPS:         " << std::setw(12) << std::fixed << std::setprecision(1)
            << peak_qps << "  req/s            ║\n";
  std::cout << "║    Min QPS:          " << std::setw(12) << std::fixed << std::setprecision(1)
            << (min_qps < 1e17 ? min_qps : 0.0) << "  req/s            ║\n";
  std::cout << "║    Avg Interval QPS: " << std::setw(12) << std::fixed << std::setprecision(1)
            << avg_interval_qps << "  req/s            ║\n";
  std::cout << "║    QPS StdDev:       " << std::setw(12) << std::fixed << std::setprecision(1)
            << qps_stddev << "  req/s            ║\n";
  std::cout << "║    QPS Stability:    " << std::setw(9) << std::fixed << std::setprecision(1)
            << stability << " %                  ║\n";

  // 变异系数
  double cv = avg_interval_qps > 0 ? (qps_stddev / avg_interval_qps * 100.0) : 0.0;
  std::cout << "║    CV (波动系数):    " << std::setw(9) << std::fixed << std::setprecision(1)
            << cv << " %                  ║\n";
  std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";

  // ================================================================
  // 分段 QPS 分析
  // ================================================================
  if (samples >= 5) {
    int seg_count = std::min(5, samples);
    int seg_size = samples / seg_count;

    std::cout << "║  [Segmented QPS Analysis — " << seg_count << " segments]\n";

    for (int seg = 0; seg < seg_count; ++seg) {
      int start = seg * seg_size;
      int end = (seg == seg_count - 1) ? samples : start + seg_size;

      double seg_sum = 0.0, seg_rate_sum = 0.0, seg_lat_sum = 0.0;
      double seg_peak = 0.0, seg_min = 1e18;
      for (int j = start; j < end; ++j) {
        seg_sum += qps_history[j];
        seg_peak = std::max(seg_peak, qps_history[j]);
        seg_min = std::min(seg_min, qps_history[j]);
        seg_rate_sum += success_rate_history[j];
        seg_lat_sum += latency_history[j];
      }
      int seg_n = end - start;
      double seg_avg = seg_sum / seg_n;
      double seg_rate = seg_rate_sum / seg_n;
      double seg_lat = seg_lat_sum / seg_n;

      // 柱状图 (占峰值比例)
      int bar_len = static_cast<int>(seg_avg / std::max(0.1, peak_qps) * 20);
      bar_len = std::min(std::max(bar_len, 1), 20);
      std::string bar(bar_len, '#');

      std::cout << "║    Seg" << (seg + 1)
                << " [" << std::setw(2) << (start + 1) << "s-" << std::setw(2) << end << "s] "
                << std::setw(8) << std::fixed << std::setprecision(1) << seg_avg
                << " QPS  │ rate " << std::setw(5) << std::setprecision(1) << seg_rate
                << "% │ lat " << std::setw(6) << std::setprecision(2) << seg_lat
                << "ms │ " << bar << " ║\n";
    }
  }
  std::cout << "╚══════════════════════════════════════════════════════════════════╝\n\n";

  // ================================================================
  // 结论与分析
  // ================================================================
  std::cout << "=== Analysis & Conclusions ===\n\n";

  std::cout << "1. Average QPS: " << std::fixed << std::setprecision(1) << overall_qps << " req/s\n"
            << "   (Effective success QPS: " << success_qps << " req/s)\n\n";

  std::cout << "2. QPS Stability: " << std::setprecision(1) << stability << "%\n";
  if (stability > 90.0)
    std::cout << "   → Excellent: QPS is very stable with minimal fluctuation.\n";
  else if (stability > 75.0)
    std::cout << "   → Good: QPS has moderate variation, acceptable for production.\n";
  else
    std::cout << "   → Warning: High QPS variance may indicate bottlenecks.\n";

  std::cout << "\n3. Peak/Min Ratio: " << std::setprecision(1)
            << (min_qps > 0 ? peak_qps / min_qps : 0.0) << "x\n";
  if (min_qps > 0 && peak_qps / min_qps < 1.3)
    std::cout << "   → Consistent throughput, no significant jitter.\n";
  else
    std::cout << "   → Throughput varies between sampling intervals.\n";

  std::cout << "\n4. Average Latency: " << std::setprecision(2) << avg_latency_ms << " ms\n";
  std::cout << "   → Theoretical max QPS (" << thread_count << " threads): "
            << std::setprecision(0) << (thread_count * 1000.0 / std::max(0.01, avg_latency_ms))
            << " req/s (Little's Law)\n";
  double efficiency = avg_latency_ms > 0.01
      ? (overall_qps * avg_latency_ms / thread_count / 1000.0 * 100.0) : 0.0;
  std::cout << "   → Concurrency Efficiency: " << std::setprecision(1) << efficiency << "%\n";

  std::cout << "\n5. Success Rate: " << std::setprecision(2) << success_rate << "%\n";
  if (success_rate > 99.9)
    std::cout << "   → Service is highly reliable.\n";
  else if (success_rate > 99.0)
    std::cout << "   → Acceptable reliability with minor failures.\n";
  else
    std::cout << "   → Failure rate needs investigation.\n";

  std::cout << "\n=== Benchmark Complete ===\n";

  return 0;
}
