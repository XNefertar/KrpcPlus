#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "../user.pb.h"
#include "xrpc/common/application.h"
#include "xrpc/common/logger.h"
#include "xrpc/monitor/runtime_stats.h"
#include "xrpc/rpc/channel.h"
#include "xrpc/rpc/controller.h"

// 发送 RPC 请求的函数，模拟客户端调用远程服务
void send_request(int thread_id, std::atomic<int>& success_count,
                  std::atomic<int>& fail_count, int requests_per_thread) {
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

  std::vector<std::thread> threads;
  std::atomic<int> success_count(0);
  std::atomic<int> fail_count(0);

  auto start_time = std::chrono::high_resolution_clock::now();

  // 启动多线程进行并发测试
  for (int i = 0; i < thread_count; i++) {
    threads.emplace_back(
        [argc, argv, i, &success_count, &fail_count, requests_per_thread]() {
          send_request(i, success_count, fail_count, requests_per_thread);
        });
  }

  // 等待所有线程执行完毕
  for (auto& t : threads) {
    t.join();
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end_time - start_time;

  LOG(INFO) << "Total requests: " << thread_count * requests_per_thread;
  LOG(INFO) << "Success count: " << success_count;
  LOG(INFO) << "Fail count: " << fail_count;
  LOG(INFO) << "Elapsed time: " << elapsed.count() << " seconds";
  LOG(INFO) << "QPS: "
            << (thread_count * requests_per_thread) / elapsed.count();

  // 打印性能报告
  RuntimeStats::GetInstance().PrintReport();

  return 0;
}
