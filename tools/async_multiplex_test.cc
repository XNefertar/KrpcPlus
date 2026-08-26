// End-to-end regression test for XRpc request-id multiplexing.
// A single connection sends all requests before the fake server replies in
// reverse order. A synchronous send/recv channel deadlocks in this scenario.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
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

constexpr int kRequestCount = 64;

bool ReadExact(int fd, void* output, size_t size) {
  char* data = static_cast<char*>(output);
  size_t read_bytes = 0;
  while (read_bytes < size) {
    const ssize_t result = recv(fd, data + read_bytes, size - read_bytes, 0);
    if (result <= 0) return false;
    read_bytes += static_cast<size_t>(result);
  }
  return true;
}

bool SendAll(int fd, const void* input, size_t size) {
  const char* data = static_cast<const char*>(input);
  size_t sent = 0;
  while (sent < size) {
    const ssize_t result = send(fd, data + sent, size - sent, 0);
    if (result <= 0) return false;
    sent += static_cast<size_t>(result);
  }
  return true;
}

class TestClosure final : public google::protobuf::Closure {
 public:
  explicit TestClosure(std::function<void()> function)
      : function_(std::move(function)) {}

  void Run() override {
    auto function = std::move(function_);
    delete this;
    function();
  }

 private:
  std::function<void()> function_;
};

bool RunReverseResponseServer(int listen_fd) {
  const int client_fd = accept(listen_fd, nullptr, nullptr);
  if (client_fd < 0) return false;

  std::vector<uint64_t> request_ids;
  request_ids.reserve(kRequestCount);
  for (int i = 0; i < kRequestCount; ++i) {
    xrpc::FixedHeader header;
    if (!ReadExact(client_fd, &header, sizeof(header))) return false;
    header.NToH();
    if (!header.IsValid() || header.total_size < xrpc::kFixedHeaderSize ||
        header.header_size > header.total_size - xrpc::kFixedHeaderSize) {
      return false;
    }

    std::vector<char> payload(header.total_size - xrpc::kFixedHeaderSize);
    if (!ReadExact(client_fd, payload.data(), payload.size())) return false;

    xrpc::RequestProtocol request_header;
    if (!request_header.ParseFromArray(payload.data(), header.header_size)) {
      return false;
    }
    request_ids.push_back(request_header.request_id());
  }

  for (auto it = request_ids.rbegin(); it != request_ids.rend(); ++it) {
    Kuser::LoginResponse login_response;
    login_response.mutable_result()->set_errcode(0);
    login_response.mutable_result()->set_errmsg(std::to_string(*it));
    login_response.set_success(true);
    std::string body;
    if (!login_response.SerializeToString(&body)) return false;

    xrpc::ResponseProtocol response_header;
    response_header.set_request_id(*it);
    response_header.set_ret_code(0);
    response_header.set_content_type(0);
    response_header.set_content_encoding(0);
    std::string encoded_header;
    if (!response_header.SerializeToString(&encoded_header)) return false;

    xrpc::FixedHeader frame_header;
    frame_header.magic = xrpc::kMagicNumber;
    frame_header.type = static_cast<uint8_t>(xrpc::MessageType::kResponse);
    frame_header.stream_type =
        static_cast<uint8_t>(xrpc::StreamType::kUnary);
    frame_header.header_size = static_cast<uint16_t>(encoded_header.size());
    frame_header.total_size = static_cast<uint32_t>(
        xrpc::kFixedHeaderSize + encoded_header.size() + body.size());
    frame_header.stream_id = static_cast<uint32_t>(*it);
    frame_header.HToN();

    if (!SendAll(client_fd, &frame_header, sizeof(frame_header)) ||
        !SendAll(client_fd, encoded_header.data(), encoded_header.size()) ||
        !SendAll(client_fd, body.data(), body.size())) {
      return false;
    }
  }

  shutdown(client_fd, SHUT_RDWR);
  close(client_fd);
  return true;
}

}  // namespace

int main() {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return 1;
  }

  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
          0 ||
      listen(listen_fd, 16) < 0) {
    perror("bind/listen");
    close(listen_fd);
    return 1;
  }

  socklen_t address_size = sizeof(address);
  getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &address_size);
  const uint16_t port = ntohs(address.sin_port);

  bool server_ok = false;
  std::thread server([&] {
    server_ok = RunReverseResponseServer(listen_fd);
    close(listen_fd);
  });

  RpcChannel channel("127.0.0.1", port, true);
  Kuser::UserServiceRpc_Stub stub(&channel);

  std::vector<std::unique_ptr<Kuser::LoginResponse>> responses;
  std::vector<std::unique_ptr<Controller>> controllers;
  responses.reserve(kRequestCount);
  controllers.reserve(kRequestCount);

  std::mutex completion_mutex;
  std::condition_variable completion_cv;
  int completed = 0;
  for (int i = 0; i < kRequestCount; ++i) {
    Kuser::LoginRequest request;
    request.set_name("multiplex-test");
    request.set_pwd(std::to_string(i));
    responses.push_back(std::make_unique<Kuser::LoginResponse>());
    controllers.push_back(std::make_unique<Controller>());

    stub.Login(controllers.back().get(), &request, responses.back().get(),
               new TestClosure([&] {
                 std::lock_guard<std::mutex> lock(completion_mutex);
                 ++completed;
                 completion_cv.notify_one();
               }));
  }

  {
    std::unique_lock<std::mutex> lock(completion_mutex);
    completion_cv.wait_for(lock, std::chrono::seconds(5),
                           [&] { return completed == kRequestCount; });
  }
  server.join();

  bool calls_ok = completed == kRequestCount;
  for (int i = 0; i < kRequestCount; ++i) {
    calls_ok = calls_ok && !controllers[i]->Failed() && responses[i]->success() &&
               responses[i]->result().errmsg() == std::to_string(i + 1);
  }
  if (!server_ok || !calls_ok) {
    std::cerr << "async multiplex test failed: completed=" << completed
              << ", server_ok=" << server_ok << '\n';
    return 1;
  }

  std::cout << "async multiplex test passed: " << completed
            << " reverse-order responses on one connection\n";
  return 0;
}
