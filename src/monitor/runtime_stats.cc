#include "xrpc/monitor/runtime_stats.h"

#include <iomanip>
#include <sstream>

RuntimeStats& RuntimeStats::GetInstance() {
  static RuntimeStats instance;
  return instance;
}

RuntimeStats::RuntimeStats() {
  for (int i = 0; i < STAGES_COUNT; ++i) {
    m_counts[i] = 0;
    m_total_times[i] = 0;
    m_max_times[i] = 0;
    for (int j = 0; j < kHistBucketCount; ++j) {
      m_hist[i][j] = 0;
    }
  }
}

void RuntimeStats::Record(Stage stage, double duration_ms) {
  m_counts[stage]++;

  // 累计纳秒
  long long ns = static_cast<long long>(duration_ms * 1'000'000.0);
  m_total_times[stage].fetch_add(ns, std::memory_order_relaxed);

  // 更新最大值 (CAS 循环)
  long long old_max = m_max_times[stage].load(std::memory_order_relaxed);
  while (ns > old_max &&
         !m_max_times[stage].compare_exchange_weak(old_max, ns,
                                                    std::memory_order_relaxed)) {
    // 重试
  }

  // 落入直方图桶
  int bucket = static_cast<int>(duration_ms / kBucketWidthMs);
  if (bucket >= kHistBucketCount) {
    bucket = kHistBucketCount - 1;  // 超范围落入最后一个桶
  }
  if (bucket < 0) bucket = 0;
  m_hist[stage][bucket].fetch_add(1, std::memory_order_relaxed);
}

RuntimeStats::PercentileResult RuntimeStats::ComputePercentiles(
    Stage stage) const {
  PercentileResult result{};
  result.count = m_counts[stage].load(std::memory_order_relaxed);
  if (result.count == 0) return result;

  // 计算 avg
  long long total_ns = m_total_times[stage].load(std::memory_order_relaxed);
  result.avg_ms = (total_ns / 1'000'000.0) / result.count;
  result.max_ms =
      m_max_times[stage].load(std::memory_order_relaxed) / 1'000'000.0;

  // 从直方图计算 P50/P95/P99
  long long p50_target = static_cast<long long>(result.count * 0.50);
  long long p95_target = static_cast<long long>(result.count * 0.95);
  long long p99_target = static_cast<long long>(result.count * 0.99);

  long long cumulative = 0;
  bool found_p50 = false, found_p95 = false, found_p99 = false;

  for (int i = 0; i < kHistBucketCount; ++i) {
    long long bucket_count =
        m_hist[stage][i].load(std::memory_order_relaxed);
    cumulative += bucket_count;

    if (!found_p50 && cumulative >= p50_target) {
      result.p50_ms = (i + 0.5) * kBucketWidthMs;  // 桶中点
      found_p50 = true;
    }
    if (!found_p95 && cumulative >= p95_target) {
      result.p95_ms = (i + 0.5) * kBucketWidthMs;
      found_p95 = true;
    }
    if (!found_p99 && cumulative >= p99_target) {
      result.p99_ms = (i + 0.5) * kBucketWidthMs;
      found_p99 = true;
      break;
    }
  }
  return result;
}

void RuntimeStats::PrintDetailedReport() {
  const char* stage_names[] = {
      "ZK_QUERY",       // 0
      "LOAD_BALANCE",   // 1 [新增]
      "CONNECT",        // 2 [新增]
      "SERIALIZE_REQ",  // 3
      "ENCODE_REQ",     // 4 [新增]
      "NET_IO",         // 5
      "DECODE_RES",     // 6 [新增]
      "DESERIALIZE_RES",// 7
      "TOTAL"           // 8
  };

  // ---- 表头 ----
  LOG(INFO) << "";
  LOG(INFO) << "╔══════════════════════════════════════════════════════════════════════════════╗";
  LOG(INFO) << "║                     XRPC Performance Statistics Report                      ║";
  LOG(INFO) << "╠══════════════════╦══════════╦══════════╦══════════╦══════════╦══════════════╣";
  LOG(INFO) << "║ Stage            ║   Count  ║  Avg ms  ║  P50 ms  ║  P95 ms  ║  P99 / Max  ║";
  LOG(INFO) << "╠══════════════════╬══════════╬══════════╬══════════╬══════════╬══════════════╣";

  // ---- 逐阶段输出 ----
  for (int i = 0; i < STAGES_COUNT; ++i) {
    auto r = ComputePercentiles(static_cast<Stage>(i));
    if (r.count == 0) continue;

    std::ostringstream line;
    line << "║ " << std::setw(16) << std::left << stage_names[i]
         << " ║ " << std::setw(8) << std::right << r.count
         << " ║ " << std::setw(8) << std::fixed << std::setprecision(3)
         << r.avg_ms
         << " ║ " << std::setw(8) << std::fixed << std::setprecision(3)
         << r.p50_ms
         << " ║ " << std::setw(8) << std::fixed << std::setprecision(3)
         << r.p95_ms;

    if (i == STAGES_COUNT - 1) {
      // TOTAL 阶段单独展示 P99 和 Max
      line << " ║ " << std::setw(5) << std::fixed << std::setprecision(3)
           << r.p99_ms << " / " << std::setw(5) << r.max_ms << " ║";
    } else {
      line << " ║ " << std::setw(12) << std::fixed << std::setprecision(3)
           << r.p99_ms << " ║";
    }
    LOG(INFO) << line.str();
  }

  LOG(INFO) << "╚══════════════════╩══════════╩══════════╩══════════╩══════════╩══════════════╝";

  // ---- 瓶颈诊断 ----
  auto total = ComputePercentiles(TOTAL);
  if (total.p50_ms > 0) {
    LOG(INFO) << "";
    LOG(INFO) << "--- Bottleneck Analysis (P50-based) ---";

    struct StageWeight {
      const char* name;
      double p50_ms;
      double pct;
    };
    StageWeight weights[STAGES_COUNT - 1];
    for (int i = 0; i < STAGES_COUNT - 1; ++i) {
      auto r = ComputePercentiles(static_cast<Stage>(i));
      weights[i] = {stage_names[i], r.p50_ms, 0.0};
      if (total.p50_ms > 0.001) {
        weights[i].pct = (r.p50_ms / total.p50_ms) * 100.0;
      }
    }

    // 按 P50 降序排列
    std::sort(weights, weights + STAGES_COUNT - 1,
              [](const StageWeight& a, const StageWeight& b) {
                return a.p50_ms > b.p50_ms;
              });

    for (const auto& w : weights) {
      if (w.p50_ms > 0.001) {
        std::ostringstream bar;
        int bar_len = static_cast<int>(w.pct / 2);  // 每 2% 一个字符
        bar_len = std::min(std::max(bar_len, 1), 50);
        for (int k = 0; k < bar_len; ++k) bar << "█";

        LOG(INFO) << "  " << std::setw(16) << std::left << w.name
                  << " " << std::setw(6) << std::right << std::fixed
                  << std::setprecision(2) << w.p50_ms << "ms | "
                  << std::setw(5) << std::fixed << std::setprecision(1)
                  << w.pct << "% " << bar.str();
      }
    }
    LOG(INFO) << "";
  }

  // ---- 长尾警告 ----
  if (total.p99_ms > total.p95_ms * 3.0 && total.p99_ms > 1.0) {
    LOG(WARNING) << "⚠️  Long-tail detected: P99 (" << std::fixed
                 << std::setprecision(2) << total.p99_ms
                 << "ms) >> P95 (" << total.p95_ms
                 << "ms), ratio=" << std::fixed << std::setprecision(1)
                 << (total.p99_ms / total.p95_ms) << "x";
  }
}
