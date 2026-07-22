// Shimming glog — 使用 stub 替代真实 glog
// 避免 macOS 上 glog 版本不兼容问题
#pragma once

#include <iostream>
#include <sstream>
#include <string>

// 模拟 LOG(severity) 宏
#define LOG(severity) \
  google::LogMessage(__FILE__, __LINE__, google::severity).stream()

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

inline void InitGoogleLogging(const char*) {}
inline void ShutdownGoogleLogging() {}

struct LogMessageVoidify {
  void operator&(const std::ostream&) {}
};

}  // namespace google

// Glog 需要的 flag 变量
static bool FLAGS_colorlogtostderr = false;
static bool FLAGS_logtostderr = true;
static int  FLAGS_minloglevel = 0;

// Logger RAII 封装
class Logger {
 public:
  explicit Logger(const char* argv0) {
    google::InitGoogleLogging(argv0);
    FLAGS_colorlogtostderr = true;
    FLAGS_logtostderr = true;
    FLAGS_minloglevel = google::INFO;
  }
  ~Logger() { google::ShutdownGoogleLogging(); }
  static void Info(const std::string& message) { LOG(INFO) << message; }
  static void Warning(const std::string& message) { LOG(WARNING) << message; }
  static void ERROR(const std::string& message) { LOG(ERROR) << message; }
  static void Fatal(const std::string& message) { LOG(FATAL) << message; }
 private:
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
};
