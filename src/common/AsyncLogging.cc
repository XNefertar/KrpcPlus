#include "AsyncLogging.h"
#include <iostream>
#include <fstream>
#include <chrono>

AsyncLogging::AsyncLogging(const std::string& file_name)
    : file_name_(file_name), running_(false) {
    // 预分配当前缓冲区的内存，避免高频分配
    current_buffer_.reserve(kBufferSize);
}

AsyncLogging::~AsyncLogging() {
    if (running_) {
        stop();
    }
}

void AsyncLogging::start() {
    running_ = true;
    thread_ = std::thread(&AsyncLogging::ThreadFunc, this);
}

void AsyncLogging::stop() {
    running_ = false;
    cond_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

// 前端写入逻辑（各个业务 EventLoop 线程调用，不能被阻塞）
void AsyncLogging::send(google::LogSeverity severity, const char* full_filename,
                        const char* base_filename, int line,
                        const struct ::tm* tm_time,
                        const char* message, size_t message_len) {
    // Glog 发过来的日志格式化，这里追加换行符
    std::string log_msg(message, message_len);
    log_msg.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    // 如果当前 buffer 未满，直接在内存中追加（纳秒级操作）
    if (current_buffer_.size() + log_msg.size() < kBufferSize) {
        current_buffer_.append(log_msg);
    } else {
        // 当前 buffer 已经满了，放入待刷盘的 buffers_ 队列中
        buffers_.push_back(std::move(current_buffer_));
        
        // 重新分配一块新的干净内存给当前 buffer
        current_buffer_.reserve(kBufferSize);
        current_buffer_.append(log_msg);
        
        // 既然有写满的 buffer 了，马上唤醒后台刷盘线程
        cond_.notify_one();
    }
}

// 后台刷盘逻辑（单独在另一根线程里慢慢执行写磁盘操作）
void AsyncLogging::ThreadFunc() {
    // 准备一个后端专用的数组，用来与前端数组交换（真正的“双缓冲区”体现）
    std::vector<std::string> buffers_to_write;
    
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 后端线程最多等待 3 秒，或者直到前端 buffers_ 有满块数据时才被唤醒
            cond_.wait_for(lock, std::chrono::seconds(3), [this] {
                return !buffers_.empty() || !running_;
            });

            // 即便 current_buffer_ 没写满，因为时间到了（3秒），也要将现有的半载数据凑进来准备刷盘
            if (!current_buffer_.empty()) {
                buffers_.push_back(std::move(current_buffer_));
                current_buffer_.reserve(kBufferSize);
            }

            // 无锁化指针/容器所有权级交换（极其快速），交换后前端的 buffers_ 变空，业务主线程不再阻塞
            buffers_to_write.swap(buffers_);
        }

        // 此时已脱离上方的临界区！前端的业务线程可以继续欢快地写 current_buffer_
        // 而这根后台线程就可以慢悠悠地执行耗时的磁盘 I/O 操作了
        flushToFile(buffers_to_write);
        buffers_to_write.clear(); // 写完后清空后端缓冲表，等待下一次交换
    }
}

void AsyncLogging::flushToFile(const std::vector<std::string>& buffers) {
    if (buffers.empty()) return;
    
    // 追加模式打开磁盘日志文件 
    std::ofstream outfile(file_name_, std::ios::app);
    if (!outfile.is_open()) {
        std::cerr << "AsyncLogging: open log file failed " << file_name_ << std::endl;
        return;
    }

    for (const auto& buf : buffers) {
        outfile.write(buf.data(), buf.size());
    }
    outfile.flush();
}