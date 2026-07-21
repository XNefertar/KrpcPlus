// ============================================================================
// RuntimeStats 全链路压测工具
// ============================================================================
// 独立运行，不依赖 ZooKeeper/Muduo。
// 模拟 9 阶段 RPC 管道的真实耗时分布，验证升级后的直方图 + 分位数统计。
//
// 编译: g++ -std=c++17 -O2 -I../src/include \
//        tools/benchmark_stats.cc src/monitor/runtime_stats.cc \
//        -o benchmark_stats -lpthread
// ============================================================================

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "xrpc/monitor/runtime_stats.h"

// 使用 stub glog (tools/include/) 提供的 LOG 宏，无需自定义

// ============================================================================
// 耗时分布模拟器
// ============================================================================
class LatencySimulator {
 public:
  LatencySimulator(uint64_t seed = 42)
      : gen_(seed),
        // ZK_QUERY: 首次查 ZK 耗时 ~2ms, 后续缓存命中 ~0.001ms
        zk_dist_(2.0, 0.8),
        // LOAD_BALANCE: atomic 操作，极快
        lb_dist_(0.001, 0.0005),
        // CONNECT: localhost 建连 ~0.5ms
        conn_dist_(0.5, 0.3),
        // SERIALIZE_REQ: 小消息 protobuf 序列化
        ser_dist_(0.015, 0.008),
        // ENCODE_REQ: 协议编码 (结构体拷贝 + htonl)
        enc_dist_(0.005, 0.003),
        // NET_IO: localhost RTT ~0.2ms, 带长尾
        net_dist_(0.2, 0.15),
        // DECODE_RES: 协议解码
        dec_dist_(0.005, 0.003),
        // DESERIALIZE_RES: 反序列化
        des_dist_(0.012, 0.006) {}

  double ZkQuery(bool cache_hit) {
    if (cache_hit) return std::abs(cache_hit_dist_(gen_));
    return std::abs(zk_dist_(gen_));
  }
  double LoadBalance()   { return std::abs(lb_dist_(gen_)); }
  double Connect()       { return std::abs(conn_dist_(gen_)); }
  double SerializeReq()  { return std::abs(ser_dist_(gen_)); }
  double EncodeReq()     { return std::abs(enc_dist_(gen_)); }
  double NetIO()         { return std::abs(net_dist_(gen_)) + longTail(); }
  double DecodeRes()     { return std::abs(dec_dist_(gen_)); }
  double DeserializeRes(){ return std::abs(des_dist_(gen_)); }

 private:
  double longTail() {
    // 3% 概率触发长尾 (模拟网络抖动 / GC 暂停)
    static std::uniform_real_distribution<> trigger(0, 1);
    if (trigger(gen_) < 0.03) {
      static std::uniform_real_distribution<> tail(5.0, 20.0);
      return tail(gen_);  // 5-20ms 额外延迟
    }
    return 0;
  }

  std::mt19937 gen_;
  std::normal_distribution<> zk_dist_;
  std::normal_distribution<> lb_dist_;
  std::normal_distribution<> conn_dist_;
  std::normal_distribution<> ser_dist_;
  std::normal_distribution<> enc_dist_;
  std::normal_distribution<> net_dist_;
  std::normal_distribution<> dec_dist_;
  std::normal_distribution<> des_dist_;
  std::normal_distribution<> cache_hit_dist_{0.001, 0.0008};
};

// ============================================================================
// 模拟一次完整 RPC 调用 (9 阶段)
// ============================================================================
void SimulateRpcCall(LatencySimulator& sim, int call_id) {
  bool cache_hit = (call_id > 0);  // 首次调用要查 ZK, 后续缓存命中

  // Stage 1: ZK_QUERY
  {
    ScopedTimer t(RuntimeStats::ZK_QUERY);
    double ms = sim.ZkQuery(cache_hit);
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 2: LOAD_BALANCE
  {
    ScopedTimer t(RuntimeStats::LOAD_BALANCE);
    double ms = sim.LoadBalance();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 3: CONNECT (仅首次)
  if (!cache_hit) {
    ScopedTimer t(RuntimeStats::CONNECT);
    double ms = sim.Connect();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 4: SERIALIZE_REQ
  {
    ScopedTimer t(RuntimeStats::SERIALIZE_REQ);
    double ms = sim.SerializeReq();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 5: ENCODE_REQ
  {
    ScopedTimer t(RuntimeStats::ENCODE_REQ);
    double ms = sim.EncodeReq();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 6: NET_IO (带长尾)
  {
    ScopedTimer t(RuntimeStats::NET_IO);
    double ms = sim.NetIO();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 7: DECODE_RES
  {
    ScopedTimer t(RuntimeStats::DECODE_RES);
    double ms = sim.DecodeRes();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }

  // Stage 8: DESERIALIZE_RES
  {
    ScopedTimer t(RuntimeStats::DESERIALIZE_RES);
    double ms = sim.DeserializeRes();
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(static_cast<long long>(ms * 1'000'000)));
  }
}

// ============================================================================
// 多线程并发压测
// ============================================================================
void WorkerThread(int thread_id, int calls_per_thread,
                  std::atomic<long long>& total_calls) {
  LatencySimulator sim(42 + thread_id * 1000);

  for (int i = 0; i < calls_per_thread; ++i) {
    ScopedTimer t(RuntimeStats::TOTAL);
    SimulateRpcCall(sim, i);
    total_calls.fetch_add(1, std::memory_order_relaxed);
  }
}

// ============================================================================
// main
// ============================================================================
int main() {
  const int kThreads = 8;
  const int kCallsPerThread = 1250;  // 8 * 1250 = 10000 calls

  std::cout << "\n=== XRPC RuntimeStats Benchmark ===\n";
  std::cout << "Threads: " << kThreads
            << " | Calls per thread: " << kCallsPerThread
            << " | Total: " << (kThreads * kCallsPerThread) << "\n";
  std::cout << "Latency model: Normal distribution + 3% long-tail spikes\n\n";

  auto start = std::chrono::high_resolution_clock::now();

  std::vector<std::thread> threads;
  std::atomic<long long> total_calls(0);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back(WorkerThread, i, kCallsPerThread,
                         std::ref(total_calls));
  }

  for (auto& t : threads) {
    t.join();
  }

  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;

  std::cout << "\n=== Benchmark Complete ===\n";
  std::cout << "Total calls: " << total_calls << "\n";
  std::cout << "Elapsed: " << elapsed.count() << "s\n";
  std::cout << "Throughput: " << static_cast<long long>(total_calls / elapsed.count())
            << " calls/s\n\n";

  // 打印全链路耗时报告
  RuntimeStats::GetInstance().PrintDetailedReport();

  return 0;
}
