#ifndef KRPC_LOG_H
#define KRPC_LOG_H
#include<glog/logging.h>
#include<string>
#include<memory>
#include"AsyncLogging.h"

//采用RAII的思想
class KrpcLogger
{
public:
      //构造函数，自动初始化glog
      explicit KrpcLogger(const char *argv0)
      {
        google::InitGoogleLogging(argv0);
        FLAGS_colorlogtostderr=true;//启用彩色日志
        FLAGS_logtostderr=false;//接管默认日志流落盘，不输出到屏幕时设为false
        FLAGS_minloglevel = google::INFO; // 过滤 INFO 级别日志，仅输出 WARNING 及以上级别
        
        // 1. 初始化我们手写的双缓冲异步日志模块（输出到krpc_async.log）
        async_sink_ = std::make_unique<AsyncLogging>("./krpc_async.log");
        // 2. 启动后台独立刷盘线程
        async_sink_->start();
        // 3. 将其注册至 Glog 中拦截所有的日志输出
        google::AddLogSink(async_sink_.get());
      }
      ~KrpcLogger(){
        if(async_sink_) {
            // 卸载拦截器并停止异步线程
            google::RemoveLogSink(async_sink_.get());
            async_sink_->stop();
        }
        google::ShutdownGoogleLogging();
      }
      //提供静态日志方法
      static void Info(const std::string &message)
      {
        LOG(INFO)<<message;
      }
      static void Warning(const std::string &message){
        LOG(WARNING)<<message;
      }
      static void ERROR(const std::string &message){
        LOG(ERROR)<<message;
      }
          static void Fatal(const std::string& message) {
        LOG(FATAL) << message;
    }
//禁用拷贝构造函数和重载赋值函数
private:
    KrpcLogger(const KrpcLogger&)=delete;
    KrpcLogger& operator=(const KrpcLogger&)=delete;

    std::unique_ptr<AsyncLogging> async_sink_;
};

#endif