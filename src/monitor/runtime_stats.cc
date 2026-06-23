#include "krpc/monitor/runtime_stats.h"

RuntimeStats& RuntimeStats::GetInstance() {
  static RuntimeStats instance;
  return instance;
}

void RuntimeStats::Record(Stage stage, double duration_ms) {
  m_counts[stage]++;
  // 以纳秒存储
  m_total_times[stage].fetch_add(
      static_cast<long long>(duration_ms * 1000000));
}

void RuntimeStats::PrintReport() {
  const char* stage_names[] = {"ZK Query", "Serialize Req", "Net I/O",
                               "Deserialize Res", "Total Call"};
  LOG(INFO) << "---------- Performance Stat Report ----------";
  for (int i = 0; i < STAGES_COUNT; ++i) {
    long long total_ns = m_total_times[i].load();
    long long count = m_counts[i].load();
    if (count > 0) {
      double avg_ms = (total_ns / 1000000.0) / count;
      LOG(INFO) << stage_names[i] << ": Avg " << avg_ms
                << " ms | Count: " << count;
    }
  }
  LOG(INFO) << "---------------------------------------------";
}
