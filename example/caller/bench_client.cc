// ============================================================================
// 真实链路 QPS 压测客户端 — 直连模式，跳过 ZK
// ============================================================================
// 通过真实 TCP socket + protobuf 序列化/反序列化进行全链路压测。
//
// 编译: 配合 CMake 构建系统编译
// 运行: ./bin/bench_client <server_ip> <server_port> [threads] [calls_per_thread]
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "xrpc/common/logger.h"

#include "../user.pb.h"
#include "xrpc/monitor/runtime_stats.h"
#include "xrpc/rpc/channel.h"
#include "xrpc/rpc/controller.h"

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

void worker(int thread_id, RpcChannel* channel,
            std::atomic<long long>& success_count,
            std::atomic<long long>& fail_count, int calls) {
  Kuser::UserServiceRpc_Stub stub(channel);

  Kuser::LoginRequest request;
  request.set_name("zhangsan");
  request.set_pwd("123456");

  Kuser::LoginResponse response;
  Controller controller;

  for (int i = 0; i < calls; ++i) {
    stub.Login(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
      fail_count++;
    } else if (response.result().errcode() == 0) {
      success_count++;
    } else {
      fail_count++;
    }

    // 重置 controller 状态供下次复用
    controller.Reset();
    response.Clear();
  }
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = true;
  FLAGS_minloglevel = google::INFO;

  if (argc < 3) {
    std::cerr << "Usage: " << argv[0]
              << " <server_ip> <server_port> [threads=100] [calls_per_thread=10000] [monitor_interval_ms=1000]\n";
    return 1;
  }

  std::string server_ip = argv[1];
  int server_port = std::atoi(argv[2]);
  int thread_count = (argc > 3) ? std::atoi(argv[3]) : 100;
  int calls_per_thread = (argc > 4) ? std::atoi(argv[4]) : 10000;
  int monitor_ms = (argc > 5) ? std::atoi(argv[5]) : 1000;

  const long long total_requests =
      static_cast<long long>(thread_count) * calls_per_thread;

  std::cout << "\n"
            << "+================================================+\n"
            << "|    XRPC Real-Link QPS Benchmark (Direct)       |\n"
            << "+================================================+\n"
            << "| Server:   " << server_ip << ":" << server_port << "\n"
            << "| Threads:  " << thread_count << "\n"
            << "| Calls/T:  " << calls_per_thread << "\n"
            << "| Total:    " << total_requests << "\n"
            << "+================================================+\n\n";

  // ---- 共享状态 ----
  std::atomic<long long> success_count(0);
  std::atomic<long long> fail_count(0);
  std::atomic<bool> running(true);

  std::vector<double> qps_history;
  std::vector<double> rate_history;

  auto start_time = Clock::now();

  // ---- 实时 QPS 监控线程 ----
  std::thread monitor([&]() {
    auto prev_time = start_time;
    long long prev_success = 0, prev_fail = 0;

    std::cout << "+------+------------+------------+------------+------------+--------------+\n"
              << "| Time | Interval   | Cumul Avg  | Success    | Fail       | Success Rate |\n"
              << "| (s)  | QPS        | QPS        | QPS        | QPS        |              |\n"
              << "+------+------------+------------+------------+------------+--------------+\n";

    while (running.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(monitor_ms));

      auto now = Clock::now();
      std::chrono::duration<double> from_start = now - start_time;
      std::chrono::duration<double> interval = now - prev_time;
      if (interval.count() < 0.001) continue;

      long long cur_s = success_count.load(std::memory_order_relaxed);
      long long cur_f = fail_count.load(std::memory_order_relaxed);

      long long d_s = cur_s - prev_success;
      long long d_f = cur_f - prev_fail;
      long long d_t = d_s + d_f;

      double iqps = d_t / interval.count();
      double cqps = (cur_s + cur_f) / std::max(0.001, from_start.count());
      double sqps = d_s / interval.count();
      double fqps = d_f / interval.count();
      double rate = d_t > 0 ? (100.0 * d_s / d_t) : 100.0;

      if (d_t > 0) {
        qps_history.push_back(iqps);
        rate_history.push_back(rate);
      }

      printf("| %4ds | %10.1f | %10.1f | %10.1f | %10.1f | %7.1f%%      |\n",
             (int)from_start.count(), iqps, cqps, sqps, fqps, rate);
      fflush(stdout);

      prev_time = now;
      prev_success = cur_s;
      prev_fail = cur_f;
    }
  });

  // ---- 启动 Worker 线程 (每个线程独立 Channel) ----
  std::vector<std::thread> workers;
  std::vector<std::unique_ptr<RpcChannel>> channels;

  for (int i = 0; i < thread_count; ++i) {
    channels.emplace_back(
        std::make_unique<RpcChannel>(server_ip, server_port, false));
  }

  // 给连接时间
  std::cout << "[Info] Connecting " << thread_count << " channels...\n";
  std::this_thread::sleep_for(std::chrono::seconds(1));

  for (int i = 0; i < thread_count; ++i) {
    workers.emplace_back(worker, i, channels[i].get(),
                         std::ref(success_count), std::ref(fail_count),
                         calls_per_thread);
  }

  for (auto& t : workers) t.join();

  running.store(false);
  auto end_time = Clock::now();
  monitor.join();

  std::chrono::duration<double> elapsed = end_time - start_time;

  long long final_s = success_count.load();
  long long final_f = fail_count.load();
  long long final_t = final_s + final_f;

  // ---- 统计 ----
  double overall_qps = final_t / elapsed.count();
  double succ_qps = final_s / elapsed.count();
  double succ_rate = final_t > 0 ? (100.0 * final_s / final_t) : 0.0;

  double peak = 0.0, min_qps = 1e18, qps_sum = 0.0;
  for (double q : qps_history) {
    peak = std::max(peak, q);
    min_qps = std::min(min_qps, q);
    qps_sum += q;
  }
  int samples = (int)qps_history.size();
  double avg_iqps = samples > 0 ? qps_sum / samples : 0.0;

  double var = 0.0;
  for (double q : qps_history) var += (q - avg_iqps) * (q - avg_iqps);
  if (samples > 1) var /= (samples - 1);
  double stddev = std::sqrt(var);
  double stability = avg_iqps > 0 ? (1.0 - stddev / avg_iqps) * 100.0 : 0.0;

  // ---- 最终报告 ----
  std::cout << "+------+------------+------------+------------+------------+--------------+\n\n";

  std::cout << "============================================================\n"
            << "        Real-Link QPS Benchmark — Final Report\n"
            << "============================================================\n"
            << " Total Requests:  " << total_requests << "\n"
            << " Completed:       " << final_t << "\n"
            << " Success:         " << final_s << "\n"
            << " Fail:            " << final_f << "\n"
            << " Elapsed:         " << std::fixed << std::setprecision(3)
            << elapsed.count() << " s\n"
            << " Success Rate:    " << std::setprecision(2) << succ_rate << "%\n"
            << "------------------------------------------------------------\n"
            << " >> Overall QPS:  " << std::setprecision(1) << overall_qps
            << " req/s\n"
            << " >> Success QPS:  " << succ_qps << " req/s\n"
            << " >> Peak QPS:     " << peak << " req/s\n"
            << " >> Min QPS:      " << (min_qps < 1e17 ? min_qps : 0.0)
            << " req/s\n"
            << " >> Avg Intv QPS: " << avg_iqps << " req/s\n"
            << " >> QPS StdDev:   " << stddev << " req/s\n"
            << " >> Stability:    " << stability << "%\n"
            << "============================================================\n\n";

  // 打印 RuntimeStats 各阶段耗时分位数
  RuntimeStats::GetInstance().PrintReport();

  return 0;
}
