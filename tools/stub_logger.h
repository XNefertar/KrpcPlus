// Stub Glog 替代 — 避免独立压测工具依赖 glog 库
#pragma once

#include <iostream>
#include <sstream>
#include <string>

// 模拟 LOG(severity) 宏
#define LOG(severity) \
  google::LogMessage(__FILE__, __LINE__, google::severity).stream()

// 模拟 LOG_IF / DLOG 等
#define LOG_IF(severity, condition) \
  !(condition) ? (void)0 : google::LogMessageVoidify() & LOG(severity)

namespace google {

enum LogSeverity { INFO, WARNING, ERROR, FATAL };

class LogMessage {
 public:
  LogMessage(const char* file, int line, LogSeverity severity)
      : severity_(severity) {
    const char* labels[] = {"I", "W", "E", "F"};
    stream_ << "[" << labels[severity] << "] ";
  }
  ~LogMessage() {
    stream_ << "\n";
    std::cout << stream_.str() << std::flush;
  }
  std::ostream& stream() { return stream_; }

 private:
  std::ostringstream stream_;
  LogSeverity severity_;
};

// Stub Init/Shutdown
inline void InitGoogleLogging(const char*) {}
inline void ShutdownGoogleLogging() {}

// 用于 LOG_IF 的空操作
struct LogMessageVoidify {
  void operator&(const std::ostream&) {}
};

}  // namespace google

// Glog 需要的 flag 变量
static bool FLAGS_colorlogtostderr = false;
static bool FLAGS_logtostderr = true;
static int  FLAGS_minloglevel = 0;
