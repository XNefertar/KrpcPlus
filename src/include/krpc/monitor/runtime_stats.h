#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>

#include "krpc/common/logger.h"

// 运行时性能统计类（单例模式），记录 RPC 调用各阶段的耗时
class RuntimeStats {
 public:
  enum Stage {
    ZK_QUERY,
    SERIALIZE_REQ,
    NET_IO,
    DESERIALIZE_RES,
    TOTAL,
    STAGES_COUNT
  };

  static RuntimeStats& GetInstance();

  void Record(Stage stage, double duration_ms);
  void PrintReport();

 private:
  RuntimeStats() {
    for (int i = 0; i < STAGES_COUNT; ++i) {
      m_counts[i] = 0;
      m_total_times[i] = 0;
    }
  }

  std::atomic<long long> m_counts[STAGES_COUNT];
  std::atomic<long long> m_total_times[STAGES_COUNT];
};

// RAII 计时器，自动在析构时上报耗时到 RuntimeStats
struct ScopedTimer {
  RuntimeStats::Stage stage;
  std::chrono::high_resolution_clock::time_point start;

  explicit ScopedTimer(RuntimeStats::Stage s)
      : stage(s), start(std::chrono::high_resolution_clock::now()) {}

  ~ScopedTimer() {
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    RuntimeStats::GetInstance().Record(stage, elapsed.count());
  }
};
