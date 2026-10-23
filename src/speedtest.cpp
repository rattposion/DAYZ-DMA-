#include "speedtest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "process.h"
#include "clock.h"
#include "telemetry.h"

namespace speed {

double quick(const Process& p, int iters) {
    tel::Suppress quiet;
    const uintptr_t base = p.module().base;
    const size_t pages = std::min<size_t>(p.module().size / 4096, 1024);
    if (!base || pages < 32) return 0;
    static uintptr_t poolBase = 0;
    static std::vector<uintptr_t> pool;
    std::vector<Process::ReadReq> rq;
    if (poolBase != base || pool.size() < 64) {
        std::vector<uint8_t> probe(pages * 8);
        for (size_t i = 0; i < pages; ++i) rq.push_back({base + i * 4096, probe.data() + i * 8, 8});
        p.readMany(rq);
        pool.clear();
        for (const auto& r : rq)
            if (r.ok && pool.size() < 256) pool.push_back(r.addr);
        poolBase = base;
        if (pool.size() < 64) return 0;
    }
    std::vector<uint8_t> buf(pool.size() * 4096);
    rq.clear();
    for (size_t i = 0; i < pool.size(); ++i) rq.push_back({pool[i], buf.data() + i * 4096, 4096});
    double okBytes = 0;
    const uint64_t t0 = qpcUs();
    for (int it = 0; it < iters; ++it) {
        for (auto& r : rq) r.ok = false;
        p.readMany(rq);
        for (const auto& r : rq) okBytes += r.ok ? 4096.0 : 0.0;
    }
    const double us = double(qpcUs() - t0);
    const double mbps = us > 0 ? okBytes / us : 0;
    if (mbps > 0) gLinkMBps = mbps;
    return mbps;
}

}
