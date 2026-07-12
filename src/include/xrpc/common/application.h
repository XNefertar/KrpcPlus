#ifndef XRPC_COMMON_APPLICATION_H_
#define XRPC_COMMON_APPLICATION_H_

#include "xrpc/common/config.h"

#include <mutex>

// 框架基础类，负责框架的初始化操作（单例模式）
class Application {
 public:
  static void Init(int argc, char** argv);
  static Application& GetInstance();
  static Config& GetConfig();

 private:
  static Config m_config;
  Application() {}
  ~Application() {}
  Application(const Application&) = delete;
  Application(Application&&) = delete;
};

#endif  // XRPC_COMMON_APPLICATION_H_
