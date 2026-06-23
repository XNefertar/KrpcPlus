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

    static KrpcStat& GetInstance();

    void Record(Stage stage, double duration_ms);

    void PrintReport();

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
