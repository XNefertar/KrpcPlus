#ifndef KRPC_RPC_CONTROLLER_H_
#define KRPC_RPC_CONTROLLER_H_

#include <google/protobuf/service.h>

#include <string>

// RPC 调用控制器，跟踪 RPC 方法调用的状态、错误信息
class Controller : public google::protobuf::RpcController {
 public:
  Controller();
  void Reset() override;
  bool Failed() const override;
  std::string ErrorText() const override;
  void SetFailed(const std::string& reason) override;

  // 以下功能暂未实现
  void StartCancel() override;
  bool IsCanceled() const override;
  void NotifyOnCancel(google::protobuf::Closure* callback) override;

 private:
  bool m_failed;         // RPC 方法执行过程中的状态
  std::string m_errText; // RPC 方法执行过程中的错误信息
};

#endif  // KRPC_RPC_CONTROLLER_H_
