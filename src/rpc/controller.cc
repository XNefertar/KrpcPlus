#include "krpc/rpc/controller.h"

// 构造函数，初始化控制器状态
Controller::Controller() {
  m_failed = false;
  m_errText = "";
}

// 重置控制器状态
void Controller::Reset() {
  m_failed = false;
  m_errText = "";
}

// 判断当前 RPC 调用是否失败
bool Controller::Failed() const { return m_failed; }

// 获取错误信息
std::string Controller::ErrorText() const { return m_errText; }

// 设置 RPC 调用失败，并记录失败原因
void Controller::SetFailed(const std::string& reason) {
  m_failed = true;
  m_errText = reason;
}

// 以下功能暂未实现
void Controller::StartCancel() {}
bool Controller::IsCanceled() const { return false; }
void Controller::NotifyOnCancel(google::protobuf::Closure* callback) {}
