#ifndef _zookeeperutil_h_
#define _zookeeperutil_h_

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <semaphore.h>
#include <zookeeper/zookeeper.h>

struct RouteNode;

//封装的zk客户端
class ZkClient
{
public:
    ZkClient();
    ~ZkClient();
    //zkclient启动连接zkserver
    void Start();
    //在zkserver中创建一个节点，根据指定的path
    void Create(const char* path,const char* data,int datalen,int state=0);
    //根据参数指定的znode节点路径，或者znode节点值
    std::string GetData(const char* path);
    std::vector<RouteNode> GetChildren(const char* path, bool watch = false);

    std::mutex cv_mutex;
    std::condition_variable cv;
    bool is_connected = false;
private:
    //Zk的客户端句柄
    zhandle_t* m_zhandle;
};
#endif
