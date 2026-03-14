#include "krpc/common/Krpcapplication.h"
#include<cstdlib>
#include<unistd.h>

Krpcconfig KrpcApplication::m_config;  // 全局配置对象

// 初始化函数，用于解析命令行参数并加载配置文件
void KrpcApplication::Init(int argc, char **argv) {
    if (argc < 2) {  // 如果命令行参数少于2个，说明没有指定配置文件
        std::cout << "格式: command -i <配置文件路径>" << std::endl;
        exit(EXIT_FAILURE);  // 退出程序
    }

    int o;
    std::string config_file;
    std::string ip_override;
    std::string port_override;

    // 使用getopt解析命令行参数
    // -i: 配置文件路径
    // -a: IP地址覆盖 (Address)
    // -p: 端口覆盖 (Port)
    while (-1 != (o = getopt(argc, argv, "i:a:p:"))) {
        switch (o) {
            case 'i':  // 如果参数是-i，后面的值就是配置文件的路径
                config_file = optarg;  // 将配置文件路径保存到config_file
                break;
            case 'a':
                ip_override = optarg;
                break;
            case 'p':
                port_override = optarg;
                break;
            case '?':  // 如果出现未知参数，提示正确格式并退出
                std::cout << "格式: command -i <配置文件路径> [-a <ip>] [-p <port>]" << std::endl;
                exit(EXIT_FAILURE);
                break;
            case ':':  // 如果选项后面没有跟参数，提示正确格式并退出
                std::cout << "格式: command -i <配置文件路径> [-a <ip>] [-p <port>]" << std::endl;
                exit(EXIT_FAILURE);
                break;
            default:
                break;
        }
    }

    // 1. 加载配置文件
    m_config.LoadConfigFile(config_file.c_str());

    // 2. 命令行参数优先级更高，如果有输入则覆盖配置
    if (!ip_override.empty()) {
        m_config.Write("rpcserverip", ip_override);
    }
    if (!port_override.empty()) {
        m_config.Write("rpcserverport", port_override);
    }
}

// 获取单例对象的引用，保证全局只有一个实例
KrpcApplication &KrpcApplication::GetInstance() {
    static KrpcApplication instance;
    return instance;
}

// 获取全局配置对象的引用
Krpcconfig& KrpcApplication::GetConfig() {
    return m_config;
}