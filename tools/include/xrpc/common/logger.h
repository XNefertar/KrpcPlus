// Stub Glog + Logger — 独立压测工具不需要真实 Glog
#pragma once

#include <iostream>
#include <sstream>
#include <string>

#define LOG(severity) \
  google::LogMessage(__FILE__, __LINE__, google::severity).stream()

#define LOG_IF(severity, condition) \
  !(condition) ? (void)0 : google::LogMessageVoidify() & LOG(severity)

namespace google {

enum LogSeverity { GLOG_INFO, GLOG_WARNING, GLOG_ERROR, GLOG_FATAL };
constexpr auto INFO = GLOG_INFO;
constexpr auto WARNING = GLOG_WARNING;
constexpr auto ERROR = GLOG_ERROR;
constexpr auto FATAL = GLOG_FATAL;

class LogMessage {
 public:
  LogMessage(const char*, int, LogSeverity severity) : severity_(severity) {
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

static bool FLAGS_colorlogtostderr = false;
static bool FLAGS_logtostderr = true;
static int  FLAGS_minloglevel = 0;
