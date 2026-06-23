#include "krpc/common/application.h"

#include <cstdlib>
#include <iostream>
#include <unistd.h>

Config Application::m_config;  // 全局配置对象

// 初始化函数，用于解析命令行参数并加载配置文件
void Application::Init(int argc, char** argv) {
  if (argc < 2) {  // 如果命令行参数少于 2 个，说明没有指定配置文件
    std::cout << "格式: command -i <配置文件路径>" << std::endl;
    exit(EXIT_FAILURE);
  }

  int o;
  std::string config_file;
  std::string ip_override;
  std::string port_override;

  // 使用 getopt 解析命令行参数
  // -i: 配置文件路径
  // -a: IP 地址覆盖 (Address)
  // -p: 端口覆盖 (Port)
  while (-1 != (o = getopt(argc, argv, "i:a:p:"))) {
    switch (o) {
      case 'i':  // -i 后的值为配置文件路径
        config_file = optarg;
        break;
      case 'a':
        ip_override = optarg;
        break;
      case 'p':
        port_override = optarg;
        break;
      case '?':  // 未知参数
        std::cout << "格式: command -i <配置文件路径> [-a <ip>] [-p <port>]"
                  << std::endl;
        exit(EXIT_FAILURE);
      case ':':  // 选项后缺参数
        std::cout << "格式: command -i <配置文件路径> [-a <ip>] [-p <port>]"
                  << std::endl;
        exit(EXIT_FAILURE);
      default:
        break;
    }
  }

  // 1. 加载配置文件
  m_config.LoadConfigFile(config_file.c_str());

  // 2. 命令行参数优先级更高，如有输入则覆盖配置
  if (!ip_override.empty()) {
    m_config.Write("rpcserverip", ip_override);
  }
  if (!port_override.empty()) {
    m_config.Write("rpcserverport", port_override);
  }
}

// 获取单例对象的引用，保证全局只有一个实例
Application& Application::GetInstance() {
  static Application instance;
  return instance;
}

// 获取全局配置对象的引用
Config& Application::GetConfig() {
  return m_config;
}
