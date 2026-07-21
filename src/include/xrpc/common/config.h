#ifndef XRPC_COMMON_CONFIG_H_
#define XRPC_COMMON_CONFIG_H_

#include <string>
#include <unordered_map>

class Config {
 public:
  // 加载配置文件
  void LoadConfigFile(const char* config_file);
  // 查找 key 对应的 value
  std::string Load(const std::string& key);
  // 手动写入/覆盖配置
  void Write(const std::string& key, const std::string& value);

 private:
  std::unordered_map<std::string, std::string> config_map;
  // 去掉字符串前后的空格
  void Trim(std::string& read_buf);
};

#endif  // XRPC_COMMON_CONFIG_H_
