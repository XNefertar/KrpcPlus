#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "../user.pb.h"
#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/monitor/runtime_stats.h"
#include "xrpc/rpc/channel.h"
#include "xrpc/rpc/controller.h"

// 发送 RPC 请求的函数，模拟客户端调用远程服务
void send_request(int thread_id, std::atomic<long long>& success_count,
                  std::atomic<long long>& fail_count, int requests_per_thread) {
  Kuser::UserServiceRpc_Stub stub(new RpcChannel(false));

  Kuser::LoginRequest request;
  request.set_name("zhangsan");
  request.set_pwd("123456");

  Kuser::LoginResponse response;
  Controller controller;

  for (int i = 0; i < requests_per_thread; ++i) {
    stub.Login(&controller, &request, &response, nullptr);

    if (controller.Failed()) {
      fail_count++;
    } else {
      if (0 == response.result().errcode()) {
        success_count++;
      } else {
        fail_count++;
      }
    }
  }
}

int main(int argc, char** argv) {
  // 初始化框架，解析命令行参数并加载配置文件
  Application::Init(argc, argv);

  // 创建日志对象
  Logger logger("MyRPC");

  const int thread_count = 100;
  const int requests_per_thread = 10000;
  const long long total_requests = thread_count * requests_per_thread;

  std::vector<std::thread> threads;
  std::atomic<long long> success_count(0);
  std::atomic<long long> fail_count(0);
  std::atomic<bool> running(true);

  // ================================================================
  // 实时 QPS 统计数据
  // ================================================================
  std::vector<double> interval_qps_history;   // 每秒 QPS 历史
  std::vector<double> interval_success_rate;  // 每秒成功率
  double peak_qps = 0.0;
  double min_qps = 1e18;
  long long qps_samples = 0;
  double qps_sum = 0.0;

  auto start_time = std::chrono::high_resolution_clock::now();

  // ---- 启动实时 QPS 监控线程 ----
  std::thread monitor_thread([&]() {
    auto prev_time = start_time;
    long long prev_success = 0;
    long long prev_fail = 0;

    LOG(INFO) << "";
    LOG(INFO) << "\u2554"
              << "══════════════════════════════════════════════════════════"
                 "════════════\u2557";
    LOG(INFO) << "\u2551              Real-time QPS Monitor Started          "
                 "                \u2551";
    LOG(INFO) << "\u2560"
              << "════════╦══════════╦══════════╦══════════╦══════════╦══════"
                 "══════════\u2563";
    LOG(INFO) << "\u2551  Time  \u2551 Interval \u2551 Cumul.   \u2551 Success  "
                 "\u2551   Fail   \u2551  Success Rate \u2551";
    LOG(INFO) << "\u2551   (s)  \u2551   QPS    \u2551   QPS    \u2551   QPS    "
                 "\u2551   QPS    \u2551               \u2551";
    LOG(INFO) << "\u2560"
              << "════════╬══════════╬══════════╬══════════╬══════════╬══════"
                 "══════════\u2563";

    while (running.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));

      auto now = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed_from_start = now - start_time;
      std::chrono::duration<double> interval_elapsed = now - prev_time;

      long long cur_success = success_count.load(std::memory_order_relaxed);
      long long cur_fail = fail_count.load(std::memory_order_relaxed);
      long long cur_total = cur_success + cur_fail;

      // 区间 QPS
      long long interval_success = cur_success - prev_success;
      long long interval_fail = cur_fail - prev_fail;
      long long interval_total = interval_success + interval_fail;

      double interval_qps = interval_total / interval_elapsed.count();
      double interval_success_qps = interval_success / interval_elapsed.count();
      double interval_fail_qps = interval_fail / interval_elapsed.count();

      // 累积平均 QPS
      double cumulative_qps =
          cur_total / std::max(0.001, elapsed_from_start.count());

      // 成功率
      double success_rate =
          interval_total > 0 ? (100.0 * interval_success / interval_total) : 0.0;

      // 记录历史
      if (interval_total > 0) {
        interval_qps_history.push_back(interval_qps);
        interval_success_rate.push_back(success_rate);
        peak_qps = std::max(peak_qps, interval_qps);
        min_qps = std::min(min_qps, interval_qps);
        qps_sum += interval_qps;
        qps_samples++;
      }

      // 打印行
      int elapsed_sec = static_cast<int>(elapsed_from_start.count());
      std::ostringstream line;
      line << "\u2551 " << std::setw(6) << std::right << elapsed_sec
           << " \u2551 " << std::setw(8) << std::right << std::fixed
           << std::setprecision(1) << interval_qps
           << " \u2551 " << std::setw(8) << std::right << std::fixed
           << std::setprecision(1) << cumulative_qps
           << " \u2551 " << std::setw(8) << std::right << std::fixed
           << std::setprecision(1) << interval_success_qps
           << " \u2551 " << std::setw(8) << std::right << std::fixed
           << std::setprecision(1) << interval_fail_qps
           << " \u2551 " << std::setw(4) << std::right << std::fixed
           << std::setprecision(1) << success_rate << "%         \u2551";
      LOG(INFO) << line.str();

      prev_time = now;
      prev_success = cur_success;
      prev_fail = cur_fail;
    }
  });

  // 启动多线程进行并发测试
  for (int i = 0; i < thread_count; i++) {
    threads.emplace_back([i, &success_count, &fail_count, requests_per_thread]() {
      send_request(i, success_count, fail_count, requests_per_thread);
    });
  }

  // 等待所有线程执行完毕
  for (auto& t : threads) {
    t.join();
  }

  // 停止监控线程
  running.store(false, std::memory_order_relaxed);

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end_time - start_time;
  monitor_thread.join();

  long long final_success = success_count.load();
  long long final_fail = fail_count.load();

  // ================================================================
  // 最终统计报告
  // ================================================================
  LOG(INFO) << "\u255a"
            << "════════╩══════════╩══════════╩══════════╩══════════╩══════"
               "══════════\u255d";
  LOG(INFO) << "";
  LOG(INFO) << "\u2554"
            << "══════════════════════════════════════════════════════════"
               "════════════\u2557";
  LOG(INFO) << "\u2551                   QPS Benchmark Final Report          "
               "              \u2551";
  LOG(INFO) << "\u2560"
            << "══════════════════════════════════════════════════════════"
               "════════════\u2563";

  double overall_qps = (final_success + final_fail) / elapsed.count();
  double success_qps = final_success / elapsed.count();
  double fail_qps = final_fail / elapsed.count();
  double overall_success_rate =
      (final_success + final_fail) > 0
          ? (100.0 * final_success / (final_success + final_fail))
          : 0.0;

  LOG(INFO) << "\u2551  Total Requests:     " << std::setw(10) << total_requests
            << "                          \u2551";
  LOG(INFO) << "\u2551  Success Count:      " << std::setw(10) << final_success
            << "                          \u2551";
  LOG(INFO) << "\u2551  Fail Count:         " << std::setw(10) << final_fail
            << "                          \u2551";
  LOG(INFO) << "\u2551  Elapsed Time:       " << std::setw(8) << std::fixed
            << std::setprecision(3) << elapsed.count()
            << " s                       \u2551";
  LOG(INFO) << "\u2560"
            << "══════════════════════════════════════════════════════════"
               "════════════\u2563";
  LOG(INFO) << "\u2551  >> Overall QPS:     " << std::setw(10) << std::fixed
            << std::setprecision(1) << overall_qps
            << "  req/s                  \u2551";
  LOG(INFO) << "\u2551  >> Success QPS:     " << std::setw(10) << std::fixed
            << std::setprecision(1) << success_qps
            << "  req/s                  \u2551";
  LOG(INFO) << "\u2551  >> Fail QPS:        " << std::setw(10) << std::fixed
            << std::setprecision(1) << fail_qps
            << "  req/s                  \u2551";
  LOG(INFO) << "\u2551  >> Success Rate:    " << std::setw(8) << std::fixed
            << std::setprecision(2) << overall_success_rate
            << " %                      \u2551";
  LOG(INFO) << "\u2560"
            << "══════════════════════════════════════════════════════════"
               "════════════\u2563";

  // ---- QPS 波动分析 ----
  if (qps_samples > 0) {
    double avg_qps = qps_sum / qps_samples;
    double variance = 0.0;
    for (const auto& q : interval_qps_history) {
      variance += (q - avg_qps) * (q - avg_qps);
    }
    variance /= qps_samples;
    double stddev_qps = std::sqrt(variance);

    LOG(INFO) << "\u2551  Peak QPS:           " << std::setw(10) << std::fixed
              << std::setprecision(1) << peak_qps
              << "  req/s                  \u2551";
    LOG(INFO) << "\u2551  Min QPS:            " << std::setw(10) << std::fixed
              << std::setprecision(1)
              << (min_qps < 1e17 ? min_qps : 0.0)
              << "  req/s                  \u2551";
    LOG(INFO) << "\u2551  Avg Interval QPS:   " << std::setw(10) << std::fixed
              << std::setprecision(1) << avg_qps
              << "  req/s                  \u2551";
    LOG(INFO) << "\u2551  QPS StdDev:         " << std::setw(10) << std::fixed
              << std::setprecision(1) << stddev_qps
              << "  req/s                  \u2551";
    LOG(INFO) << "\u2551  QPS Stability:      " << std::setw(8) << std::fixed
              << std::setprecision(1)
              << (avg_qps > 0 ? (1.0 - stddev_qps / avg_qps) * 100.0 : 0.0)
              << " %                       \u2551";
    LOG(INFO) << "\u2560"
              << "══════════════════════════════════════════════════════════"
                 "════════════\u2563";

    // 分段 QPS 分析 (将测试过程分成 5 段)
    int segments = 5;
    int samples_per_segment =
        std::max(1, static_cast<int>(qps_samples) / segments);
    LOG(INFO) << "\u2551  Segmented QPS Analysis ("
              << std::min(segments, static_cast<int>(qps_samples))
              << " segments):                    \u2551";
    for (int seg = 0; seg < segments && seg * samples_per_segment < qps_samples;
         ++seg) {
      int start_idx = seg * samples_per_segment;
      int end_idx =
          std::min(start_idx + samples_per_segment,
                   static_cast<int>(interval_qps_history.size()));
      double seg_sum = 0.0;
      double seg_success_sum = 0.0;
      int seg_count = 0;
      for (int j = start_idx; j < end_idx; ++j) {
        seg_sum += interval_qps_history[j];
        seg_success_sum += interval_success_rate[j];
        seg_count++;
      }
      double seg_avg_qps = seg_count > 0 ? seg_sum / seg_count : 0.0;
      double seg_avg_rate = seg_count > 0 ? seg_success_sum / seg_count : 0.0;
      int seg_start_sec = start_idx + 1;
      int seg_end_sec = end_idx;

      std::ostringstream bar;
      int bar_len = static_cast<int>(seg_avg_qps / (std::max(0.1, peak_qps) / 20));
      bar_len = std::min(std::max(bar_len, 1), 20);
      for (int k = 0; k < bar_len; ++k) bar << "#";

      LOG(INFO) << "\u2551  Seg " << (seg + 1) << " [" << std::setw(2)
                << seg_start_sec << "s-" << std::setw(2) << seg_end_sec
                << "s]: " << std::setw(8) << std::fixed << std::setprecision(1)
                << seg_avg_qps << " QPS, rate=" << std::setw(5)
                << std::setprecision(1) << seg_avg_rate << "% " << bar.str()
                << "  \u2551";
    }
  }

  LOG(INFO) << "\u255a"
            << "══════════════════════════════════════════════════════════"
               "════════════\u255d";
  LOG(INFO) << "";

  // 打印 RuntimeStats 详细性能报告 (各阶段耗时分位数)
  RuntimeStats::GetInstance().PrintReport();

  return 0;
}
