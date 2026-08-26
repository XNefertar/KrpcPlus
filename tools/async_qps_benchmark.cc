// Real loopback-TCP benchmark for one-inflight versus multiplexed XRpc calls.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "example/user.pb.h"
#include "xrpc/codec/xrpc_codec.h"
#include "xrpc/protocol/xrpc_protocol.pb.h"
#include "xrpc/rpc/channel.h"
#include "xrpc/rpc/controller.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kServiceDelay = std::chrono::microseconds(250);

bool ReadExact(int fd, void* output, size_t size) {
  char* data = static_cast<char*>(output);
  size_t total = 0;
  while (total < size) {
    const ssize_t result = recv(fd, data + total, size - total, 0);
    if (result <= 0) return false;
    total += static_cast<size_t>(result);
  }
  return true;
}

bool SendAll(int fd, const void* input, size_t size) {
  const char* data = static_cast<const char*>(input);
  size_t total = 0;
  while (total < size) {
    const ssize_t result = send(fd, data + total, size - total, 0);
    if (result <= 0) return false;
    total += static_cast<size_t>(result);
  }
  return true;
}

struct QueuedResponse {
  uint64_t request_id;
  uint32_t stream_id;
  Clock::time_point ready_at;
};

bool SendResponse(int fd, const QueuedResponse& queued,
                  const std::string& body) {
  xrpc::ResponseProtocol response;
  response.set_request_id(queued.request_id);
  response.set_ret_code(0);
  response.set_content_type(0);
  response.set_content_encoding(0);
  std::string encoded_header;
  if (!response.SerializeToString(&encoded_header)) return false;

  xrpc::FixedHeader header;
  header.magic = xrpc::kMagicNumber;
  header.type = static_cast<uint8_t>(xrpc::MessageType::kResponse);
  header.stream_type = static_cast<uint8_t>(xrpc::StreamType::kUnary);
  header.header_size = static_cast<uint16_t>(encoded_header.size());
  header.total_size = static_cast<uint32_t>(
      xrpc::kFixedHeaderSize + encoded_header.size() + body.size());
  header.stream_id = queued.stream_id;
  header.HToN();

  return SendAll(fd, &header, sizeof(header)) &&
         SendAll(fd, encoded_header.data(), encoded_header.size()) &&
         SendAll(fd, body.data(), body.size());
}

void HandleConnection(int fd) {
  Kuser::LoginResponse login_response;
  login_response.mutable_result()->set_errcode(0);
  login_response.set_success(true);
  std::string response_body;
  if (!login_response.SerializeToString(&response_body)) {
    close(fd);
    return;
  }

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<QueuedResponse> queue;
  bool input_closed = false;

  std::thread responder([&] {
    while (true) {
      QueuedResponse response{};
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [&] { return input_closed || !queue.empty(); });
        if (queue.empty()) {
          if (input_closed) break;
          continue;
        }
        response = queue.front();
        queue.pop_front();
      }
      std::this_thread::sleep_until(response.ready_at);
      if (!SendResponse(fd, response, response_body)) break;
    }
  });

  while (true) {
    xrpc::FixedHeader header;
    if (!ReadExact(fd, &header, sizeof(header))) break;
    header.NToH();
    if (!header.IsValid() || header.total_size < xrpc::kFixedHeaderSize ||
        header.header_size > header.total_size - xrpc::kFixedHeaderSize) {
      break;
    }

    std::vector<char> payload(header.total_size - xrpc::kFixedHeaderSize);
    if (!ReadExact(fd, payload.data(), payload.size())) break;
    xrpc::RequestProtocol request;
    if (!request.ParseFromArray(payload.data(), header.header_size)) break;

    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      queue.push_back(
          {request.request_id(), header.stream_id, Clock::now() + kServiceDelay});
    }
    queue_cv.notify_one();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    input_closed = true;
  }
  queue_cv.notify_all();
  responder.join();
  shutdown(fd, SHUT_RDWR);
  close(fd);
}

class BenchmarkServer {
 public:
  bool Start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0 ||
        listen(listen_fd_, 16) < 0) {
      return false;
    }
    socklen_t size = sizeof(address);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &size);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([&] {
      for (int i = 0; i < 2; ++i) {
        const int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) break;
        HandleConnection(fd);
      }
    });
    return true;
  }

  ~BenchmarkServer() {
    if (listen_fd_ >= 0) close(listen_fd_);
    if (thread_.joinable()) thread_.join();
  }

  uint16_t port() const { return port_; }

 private:
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
};

class FunctionClosure final : public google::protobuf::Closure {
 public:
  explicit FunctionClosure(std::function<void()> function)
      : function_(std::move(function)) {}
  void Run() override {
    auto function = std::move(function_);
    delete this;
    function();
  }

 private:
  std::function<void()> function_;
};

struct Result {
  const char* name;
  int calls;
  int success;
  double seconds;
  double p50_ms;
  double p99_ms;

  double qps() const { return calls / seconds; }
};

void FillPercentiles(std::vector<double>& latency, Result& result) {
  std::sort(latency.begin(), latency.end());
  result.p50_ms = latency[latency.size() * 50 / 100];
  result.p99_ms = latency[latency.size() * 99 / 100];
}

Result RunSynchronous(uint16_t port, int calls) {
  RpcChannel channel("127.0.0.1", port, true);
  Kuser::UserServiceRpc_Stub stub(&channel);
  Kuser::LoginRequest request;
  request.set_name("benchmark");
  request.set_pwd("sync");
  std::vector<double> latency;
  latency.reserve(calls);
  int success = 0;
  const auto begin = Clock::now();
  for (int i = 0; i < calls; ++i) {
    Kuser::LoginResponse response;
    Controller controller;
    const auto call_begin = Clock::now();
    stub.Login(&controller, &request, &response, nullptr);
    latency.push_back(std::chrono::duration<double, std::milli>(
                          Clock::now() - call_begin)
                          .count());
    if (!controller.Failed() && response.success()) ++success;
  }
  Result result{"single-inflight", calls, success,
                std::chrono::duration<double>(Clock::now() - begin).count(), 0,
                0};
  FillPercentiles(latency, result);
  return result;
}

Result RunMultiplexed(uint16_t port, int calls, int max_inflight) {
  RpcChannel channel("127.0.0.1", port, true);
  Kuser::UserServiceRpc_Stub stub(&channel);
  std::vector<std::unique_ptr<Kuser::LoginResponse>> responses(calls);
  std::vector<std::unique_ptr<Controller>> controllers(calls);
  std::vector<double> latency(calls);

  std::mutex mutex;
  std::condition_variable cv;
  int completed = 0;
  int success = 0;
  const auto begin = Clock::now();
  for (int i = 0; i < calls; ++i) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&] { return i - completed < max_inflight; });
    }

    Kuser::LoginRequest request;
    request.set_name("benchmark");
    request.set_pwd("multiplexed");
    responses[i] = std::make_unique<Kuser::LoginResponse>();
    controllers[i] = std::make_unique<Controller>();
    const auto call_begin = Clock::now();
    stub.Login(controllers[i].get(), &request, responses[i].get(),
               new FunctionClosure([&, i, call_begin] {
                 const double elapsed =
                     std::chrono::duration<double, std::milli>(Clock::now() -
                                                               call_begin)
                         .count();
                 std::lock_guard<std::mutex> lock(mutex);
                 latency[i] = elapsed;
                 if (!controllers[i]->Failed() && responses[i]->success()) {
                   ++success;
                 }
                 ++completed;
                 cv.notify_all();
               }));
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return completed == calls; });
  }
  Result result{"multiplexed", calls, success,
                std::chrono::duration<double>(Clock::now() - begin).count(), 0,
                0};
  FillPercentiles(latency, result);
  return result;
}

void Print(const Result& result) {
  std::cout << std::left << std::setw(18) << result.name << std::right
            << " QPS=" << std::setw(10) << std::fixed << std::setprecision(1)
            << result.qps() << " success=" << result.success << "/"
            << result.calls << " P50=" << std::setprecision(3)
            << result.p50_ms << "ms P99=" << result.p99_ms << "ms\n";
}

}  // namespace

int main(int argc, char** argv) {
  const int calls = argc > 1 ? std::max(100, std::atoi(argv[1])) : 20000;
  const int inflight = argc > 2 ? std::max(1, std::atoi(argv[2])) : 256;
  BenchmarkServer server;
  if (!server.Start()) {
    perror("benchmark server");
    return 1;
  }

  const Result synchronous = RunSynchronous(server.port(), calls);
  const Result multiplexed = RunMultiplexed(server.port(), calls, inflight);
  Print(synchronous);
  Print(multiplexed);
  std::cout << "QPS improvement: " << std::fixed << std::setprecision(2)
            << multiplexed.qps() / synchronous.qps() << "x\n";
  return synchronous.success == calls && multiplexed.success == calls ? 0 : 1;
}
