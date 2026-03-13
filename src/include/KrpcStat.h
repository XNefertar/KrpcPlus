#pragma once
#include <chrono>
#include <atomic>
#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include "KrpcLogger.h"

class KrpcStat {
public:
    enum Stage {
        ZK_QUERY,
        SERIALIZE_REQ,
        NET_IO,
        DESERIALIZE_RES,
        TOTAL,
        STAGES_COUNT
    };

    static KrpcStat& GetInstance() {
        static KrpcStat instance;
        return instance;
    }

    void Record(Stage stage, double duration_ms) {
        m_counts[stage]++;
        m_total_times[stage].fetch_add(static_cast<long long>(duration_ms * 1000000)); // store in nanoseconds
    }

    void PrintReport() {
        const char* stage_names[] = {"ZK Query", "Serialize Req", "Net I/O", "Deserialize Res", "Total Call"};
        LOG(INFO) << "---------- Performance Stat Report ----------";
        for (int i = 0; i < STAGES_COUNT; ++i) {
            long long total_ns = m_total_times[i].load();
            long long count = m_counts[i].load();
            if (count > 0) {
                double avg_ms = (total_ns / 1000000.0) / count;
                LOG(INFO) << stage_names[i] << ": Avg " << avg_ms << " ms | Count: " << count;
            }
        }
        LOG(INFO) << "---------------------------------------------";
    }

private:
    KrpcStat() {
        for(int i=0; i<STAGES_COUNT; ++i) {
            m_counts[i] = 0;
            m_total_times[i] = 0;
        }
    }
    std::atomic<long long> m_counts[STAGES_COUNT];
    std::atomic<long long> m_total_times[STAGES_COUNT];
};

struct ScopedTimer {
    KrpcStat::Stage stage;
    std::chrono::high_resolution_clock::time_point start;
    ScopedTimer(KrpcStat::Stage s) : stage(s), start(std::chrono::high_resolution_clock::now()) {}
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        KrpcStat::GetInstance().Record(stage, elapsed.count());
    }
};
