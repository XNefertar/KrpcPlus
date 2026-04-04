#ifndef _KrpcRouteManager_H__
#define _KrpcRouteManager_H__

#include <iostream>


#include <mutex>
#include <memory>
#include <thread>
#include <string>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <unordered_map>

class ZkClient; // 前向声明
class LoadBalancer;

enum NODE_STATUS {
    Unknown,
    Normal,
    Error,
};

struct RouteNode {
    std::string _address;
    NODE_STATUS _status;

    RouteNode(std::string address = "", NODE_STATUS status = Unknown) : _address(address), _status(status) {}

    bool isAlive() const { return _status == Normal; }
    bool isAddressDivisible() { return _address.find(':') != -1; }

    NODE_STATUS FetchStatus() { return _status; }
    std::string FetchAddress() const { return _address; }
    std::string FetchIPAddress() {
        int idx = _address.find(":");
        return _address.substr(0, idx);
    }
    std::string FetchPort() {
        int idx = _address.find(":");
        return _address.substr(idx+1);
    }
};

class RouteManager {
    using RouteTable = std::unordered_map<std::string, std::vector<RouteNode>>;
private:
    RouteManager();

    std::shared_ptr<const RouteTable> _curRouteTable;
    ZkClient *_zkClient;
    std::mutex _writeMutex;
    std::thread _healthCheck;
    std::unique_ptr<LoadBalancer> _loadBalancer;

    bool checkTcpAlive(const std::string& ip, int port) {
        std::cout << "CHECKTCPALIVE STARTING" << std::endl;
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return false;

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

        int res = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        close(sockfd);
        return res == 0;
    }

    void HealthCheckTask() {
        prctl(PR_SET_NAME, "KrpcHealthCheck", 0, 0, 0); 
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cout << "HEALTHCHECKTASK STARTING" << std::endl;

            std::shared_ptr<const RouteTable> local_table = std::atomic_load(&_curRouteTable);
            for (const auto& pair : *local_table) {
                const std::string& path = pair.first;
                for (const auto& node : pair.second) {
                    size_t pos = node._address.find(':');
                    std::string ip = node._address.substr(0, pos);
                    int port = stoi(node._address.substr(pos+1));

                    bool isAlive = checkTcpAlive(ip, port);
                    std::cout << "ISALIVE: " << std::boolalpha << isAlive << std::noboolalpha << std::endl;
                    std::cout << "NODE_STATUS" << node._status << std::endl;
                    if (isAlive && node._status != Normal) {
                        std::cout << "recover error node address" << node.FetchAddress() << std::endl;
                        MarkNodeStatus(path, node._address, Normal);
                    } else if (!isAlive && node._status != Error) {
                        std::cout << "destroy normal node" << std::endl;
                        MarkNodeStatus(path, node._address, Error);
                    }
                }
            }
        }
    }
public:
    std::vector<RouteNode> GetRouteNodes(const std::string& path) const;
    std::vector<RouteNode> GetHealthNodes(const std::string& path) const;
    void UpdateRouteTable(const std::string& path, ZkClient* client, bool watch = false);
    void MarkNodeStatus(const std::string& path, const std::string& address, NODE_STATUS target_status);
    static RouteManager* GetInstance();
    LoadBalancer* GetLoadBalancer() const { return _loadBalancer.get(); }
    ZkClient* GetZkClient() const { return _zkClient; }
};

#endif