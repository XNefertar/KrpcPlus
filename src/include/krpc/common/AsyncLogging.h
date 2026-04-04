#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <glog/logging.h>

// 继承 google::LogSink，接管 glog 的日志输出
class AsyncLogging : public google::LogSink {
public:
    // 初始化异步日志并指定输出文件
    AsyncLogging(const std::string& file_name);
    ~AsyncLogging();

    // 重写 LogSink 的 send 方法，前端业务线程调用此方法输出日志
    void send(google::LogSeverity severity, const char* full_filename,
              const char* base_filename, int line,
              const struct ::tm* tm_time,
              const char* message, size_t message_len) override;

    // 启动后台刷盘线程
    void start();
    // 停止后台刷盘线程
    void stop();

private:
    // 后台专门用于刷盘的线程函数
    void ThreadFunc();
    // 将写满的缓冲区数据物理落盘
    void flushToFile(const std::vector<std::string>& buffers);

    std::string file_name_;
    std::atomic<bool> running_;
    std::thread thread_;

    std::mutex mutex_;
    std::condition_variable cond_;

    // 双缓冲队列设计
    // current_buffer_：当前正在写入的缓冲（前端使用）
    // buffers_：已经写满，等待后台线程刷盘的缓冲队列（前后端交互）
    std::string current_buffer_;
    std::vector<std::string> buffers_;

    // 单个缓冲区大小设定为 4MB
    static const size_t kBufferSize = 1024 * 1024 * 4; 
};
