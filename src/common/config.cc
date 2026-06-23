#include "krpc/common/config.h"

#include <memory>

// 加载配置文件，解析配置文件中的键值对
void Config::LoadConfigFile(const char* config_file) {
  // 使用智能指针管理文件指针，确保文件在退出时自动关闭
  std::unique_ptr<FILE, decltype(&fclose)> pf(fopen(config_file, "r"), &fclose);

  if (pf == nullptr) {  // 文件打开失败
    exit(EXIT_FAILURE);
  }

  char buf[1024];  // 存储从文件中读取的每一行内容
  while (fgets(buf, 1024, pf.get()) != nullptr) {
    std::string read_buf(buf);
    Trim(read_buf);  // 去掉字符串前后的空格

    // 忽略注释行（以 # 开头）和空行
    if (read_buf[0] == '#' || read_buf.empty()) continue;

    // 查找键值对的分隔符 '='
    int index = read_buf.find('=');
    if (index == -1) continue;  // 没找到 '='，跳过该行

    // 提取 key
    std::string key = read_buf.substr(0, index);
    Trim(key);

    // 查找行尾的换行符
    int endindex = read_buf.find('\n', index);
    // 提取 value，去掉换行符
    std::string value = read_buf.substr(index + 1, endindex - index - 1);
    Trim(value);

    // 将键值对存入配置 map
    config_map.insert({key, value});
  }
}

// 根据 key 查找对应的 value
std::string Config::Load(const std::string& key) {
  auto it = config_map.find(key);
  if (it == config_map.end()) {
    return "";  // 未找到返回空字符串
  }
  return it->second;
}

void Config::Write(const std::string& key, const std::string& value) {
  config_map[key] = value;
}

// 去掉字符串前后的空格
void Config::Trim(std::string& read_buf) {
  int index = read_buf.find_first_not_of(' ');
  if (index != -1) {
    read_buf = read_buf.substr(index, read_buf.size() - index);
  }

  index = read_buf.find_last_not_of(' ');
  if (index != -1) {
    read_buf = read_buf.substr(0, index + 1);
  }
}
