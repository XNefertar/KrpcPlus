#ifndef XRPC_COMMON_LOGGER_H_
#define XRPC_COMMON_LOGGER_H_

#include <glog/logging.h>

#include <string>

// 采用 RAII 思想封装 glog 日志系统
class Logger {
 public:
  // 构造函数，自动初始化 glog
  explicit Logger(const char* argv0) {
    google::InitGoogleLogging(argv0);
    FLAGS_colorlogtostderr = true;   // 启用彩色日志
    FLAGS_logtostderr = true;        // 默认输出标准错误
    FLAGS_minloglevel = google::INFO; // 过滤 INFO 级别日志，仅输出 WARNING 及以上级别
  }

  ~Logger() { google::ShutdownGoogleLogging(); }

  // 提供静态日志方法
  static void Info(const std::string& message) { LOG(INFO) << message; }
  static void Warning(const std::string& message) { LOG(WARNING) << message; }
  static void ERROR(const std::string& message) { LOG(ERROR) << message; }
  static void Fatal(const std::string& message) { LOG(FATAL) << message; }

  // 禁用拷贝
 private:
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;
};

#endif  // XRPC_COMMON_LOGGER_H_
