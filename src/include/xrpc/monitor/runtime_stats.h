#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "xrpc/common/logger.h"

// ============================================================================
// RuntimeStats — 运行时性能统计（单例模式）
// ============================================================================
// 升级内容:
//   1. 新增 4 个阶段: ENCODE_REQ, DECODE_RES, LOAD_BALANCE, CONNECT
//   2. 固定桶直方图 → P50/P95/P99/Max 精确计算
//   3. PrintReport → PrintDetailedReport (含分位数 + 瓶颈诊断)
// ============================================================================

class RuntimeStats {
 public:
  // ---- 阶段枚举 (9 个阶段完整覆盖全链路) ----
  enum Stage {
    ZK_QUERY = 0,         // ZooKeeper 查询路由
    LOAD_BALANCE,         // [新增] 负载均衡选节点
    CONNECT,              // [新增] TCP 建连
    SERIALIZE_REQ,        // 序列化请求体
    ENCODE_REQ,           // [新增] 协议编码 (Legacy/XRpc)
    NET_IO,               // 网络收发
    DECODE_RES,           // [新增] 协议解码
    DESERIALIZE_RES,      // 反序列化响应体
    TOTAL,                // 总耗时
    STAGES_COUNT
  };

  // ---- 单例 ----
  static RuntimeStats& GetInstance();

  // ---- 记录一次耗时 ----
  void Record(Stage stage, double duration_ms);

  // ---- 打印完整报告 (含分位数) ----
  void PrintDetailedReport();
  void PrintReport() { PrintDetailedReport(); }  // 兼容旧接口

 private:
  RuntimeStats();

  // ========== 直方图 (固定桶) ==========
  // 200 桶 × 0.1ms = 0..20ms 范围，覆盖绝大多数 RPC 调用
  static constexpr int kHistBucketCount = 300;
  static constexpr double kBucketWidthMs = 0.1;     // 每桶 0.1ms
  static constexpr double kHistMaxMs = kHistBucketCount * kBucketWidthMs; // 30ms

  std::atomic<long long> m_counts[STAGES_COUNT];
  std::atomic<long long> m_total_times[STAGES_COUNT];  // 纳秒
  std::atomic<long long> m_max_times[STAGES_COUNT];    // 最大耗时 (纳秒) [新增]
  std::atomic<long long> m_hist[STAGES_COUNT][kHistBucketCount]; // 每阶段独立直方图 [新增]

  // ---- 辅助: 固定桶直方图计算 ----
  struct PercentileResult {
    double p50_ms;
    double p95_ms;
    double p99_ms;
    double max_ms;
    double avg_ms;
    long long count;
  };
  PercentileResult ComputePercentiles(Stage stage) const;
};

// ============================================================================
// RAII 计时器 — 构造开始计时，析构自动上报
// ============================================================================
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
