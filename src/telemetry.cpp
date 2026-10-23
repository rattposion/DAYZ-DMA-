#include "telemetry.h"

#include "clock.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace tel {
namespace {

constexpr int kHist = 50;
constexpr int kWindow = 10;

struct TimerAcc {
    std::atomic<uint64_t> n{0}, sum{0}, mx{0};
    std::atomic<uint32_t> h[kHist];
};
struct TimerSnap {
    uint64_t n = 0, sum = 0, mx = 0;
    uint32_t h[kHist] = {};
};
struct Second {
    double dt = 1;
    TimerSnap t[T_COUNT];
    uint64_t c[C_COUNT] = {};
    double cpuCore = 0;
};

struct State {
    TimerAcc timers[T_COUNT];
    std::atomic<uint64_t> counters[C_COUNT];

    std::deque<Second> ring;

    std::mutex discoverMx;
    int discoverReq = 0;
    std::string discoverName;

    std::thread sampler;
    std::mutex wakeMx;
    std::condition_variable wake;
    bool stopping = false;
    void (*logSink)(const char*) = nullptr;

    State() {
        for (auto& c : counters) c.store(0);
        for (auto& t : timers)
            for (auto& h : t.h) h.store(0);
    }
};

State& S() {
    static State* s = new State();
    return *s;
}

thread_local int tlSuppress = 0;

std::string fmt(const char* f, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

int bucketOf(uint64_t us) {
    if (us < 2) return 0;
    unsigned long lg;
    _BitScanReverse64(&lg, us);
    const int b = 2 * int(lg) + int((us >> (lg - 1)) & 1);
    return std::min(b, kHist - 1);
}
double bucketUb(int b) {
    if (b <= 0) return 1;
    return (b & 1) ? std::ldexp(1.0, (b + 1) / 2) : 1.5 * std::ldexp(1.0, b / 2);
}

void atomicMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t m = a.load(std::memory_order_relaxed);
    while (v > m && !a.compare_exchange_weak(m, v, std::memory_order_relaxed)) {}
}

double pctMs(const TimerSnap& t, double p) {
    uint64_t total = 0;
    for (uint32_t h : t.h) total += h;
    if (!total) return 0;
    const uint64_t rank = std::max<uint64_t>(1, uint64_t(std::ceil(p * double(total))));
    uint64_t cum = 0;
    for (int b = 0; b < kHist; ++b) {
        cum += t.h[b];
        if (cum >= rank) return std::min(bucketUb(b), double(t.mx)) / 1000.0;
    }
    return double(t.mx) / 1000.0;
}
double avgMs(const TimerSnap& t) { return t.n ? double(t.sum) / double(t.n) / 1000.0 : 0; }
double maxMs(const TimerSnap& t) { return double(t.mx) / 1000.0; }

struct Agg {
    TimerSnap t[T_COUNT];
    uint64_t c[C_COUNT] = {};
    double secs = 0, cpuCore = 0;
    int n = 0;
};

void addSnap(TimerSnap& d, const TimerSnap& s) {
    d.n += s.n;
    d.sum += s.sum;
    d.mx = std::max(d.mx, s.mx);
    for (int i = 0; i < kHist; ++i) d.h[i] += s.h[i];
}

Agg aggregate(const std::deque<Second>& ring) {
    Agg a;
    for (const Second& s : ring) {
        for (int t = 0; t < T_COUNT; ++t) addSnap(a.t[t], s.t[t]);
        for (int c = 0; c < C_COUNT; ++c) a.c[c] += s.c[c];
        a.secs += s.dt;
        a.cpuCore += s.cpuCore;
        ++a.n;
    }
    if (a.n) a.cpuCore /= a.n;
    return a;
}

double filetimeToSec(const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; }

void sampleLoop() {
    State& s = S();
    uint64_t prevC[C_COUNT] = {};
    FILETIME c0, e0, k0, u0;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    double prevProcCpu = filetimeToSec(k0) + filetimeToSec(u0);
    auto prevT = std::chrono::steady_clock::now();
    auto lastSink = prevT;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.wakeMx);
            if (s.wake.wait_for(lk, std::chrono::seconds(1), [&] { return s.stopping; })) return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - prevT).count();
        prevT = now;

        Second sec;
        sec.dt = dt;
        for (int t = 0; t < T_COUNT; ++t) {
            TimerAcc& a = s.timers[t];
            TimerSnap& d = sec.t[t];
            d.n = a.n.exchange(0, std::memory_order_relaxed);
            d.sum = a.sum.exchange(0, std::memory_order_relaxed);
            d.mx = a.mx.exchange(0, std::memory_order_relaxed);
            for (int i = 0; i < kHist; ++i) d.h[i] = a.h[i].exchange(0, std::memory_order_relaxed);
        }
        for (int c = 0; c < C_COUNT; ++c) {
            const uint64_t cur = s.counters[c].load(std::memory_order_relaxed);
            sec.c[c] = cur - prevC[c];
            prevC[c] = cur;
        }
        {
            FILETIME c, e, k, u;
            GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
            const double cpu = filetimeToSec(k) + filetimeToSec(u);
            sec.cpuCore = dt > 0 ? (cpu - prevProcCpu) / dt * 100.0 : 0;
            prevProcCpu = cpu;
        }

        s.ring.push_back(sec);
        if (int(s.ring.size()) > kWindow) s.ring.pop_front();

        if (s.logSink && now - lastSink >= std::chrono::seconds(10)) {
            lastSink = now;
            const Agg a = aggregate(s.ring);
            const double W = std::max(1.0, a.secs);
            const std::string line =
                fmt("[log] tel: %.0f fps, scan %.0f/s (avg %.1f ms), calls/s %.0f, call p50 %.2f p95 %.2f max %.1f ms, link busy %.0f%%, lock p95 %.2f ms, "
                    "snap age p95 %.0f ms, fail %.2f%%, cpu %.0f%%",
                    double(a.c[C_FRAMES]) / W, double(a.c[C_SCANS]) / W, avgMs(a.t[T_SCAN]), double(a.c[C_CALLS]) / W, pctMs(a.t[T_CALL], 0.5),
                    pctMs(a.t[T_CALL], 0.95), maxMs(a.t[T_CALL]),
                    std::min(1.0, double(a.t[T_DEVICE].sum + a.t[T_XLATE].sum + a.t[T_VIRT].sum) / 1e6 / W) * 100, pctMs(a.t[T_LOCK], 0.95),
                    pctMs(a.t[T_SNAPAGE], 0.95), a.c[C_REQS] ? 100.0 * double(a.c[C_REQS_FAIL]) / double(a.c[C_REQS]) : 0.0, a.cpuCore);
            s.logSink(line.c_str());
        }
    }
}

}

uint64_t nowUs() { return qpcUs(); }

void time(Timer t, uint64_t us) {
    if (tlSuppress) return;
    TimerAcc& a = S().timers[t];
    a.n.fetch_add(1, std::memory_order_relaxed);
    a.sum.fetch_add(us, std::memory_order_relaxed);
    a.h[bucketOf(us)].fetch_add(1, std::memory_order_relaxed);
    atomicMax(a.mx, us);
}
void count(Counter c, uint64_t n) {
    if (tlSuppress) return;
    S().counters[c].fetch_add(n, std::memory_order_relaxed);
}

void call(const CallInfo& c) {
    if (tlSuppress) return;
    time(T_CALL, c.totalUs);
    if (c.single) {
        time(T_READ1, c.totalUs);
        count(C_SINGLES);
    }
    time(T_LOCK, c.lockUs);
    if (c.lockUs >= 100) count(C_LOCK_WAITS);
    count(C_CALLS);
    count(C_REQS, c.reqs);
    count(C_REQS_FAIL, c.reqs - c.reqsOk);
    count(C_BYTES, c.bytes);
    count(C_BYTES_OK, c.bytesOk);
    if (c.totalUs > 40000) count(C_STALLS);
}

Suppress::Suppress() { ++tlSuppress; }
Suppress::~Suppress() { --tlSuppress; }

void requestDiscover(const std::string& name) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.discoverMx);
    s.discoverReq = 1;
    s.discoverName = name;
}
bool discoverRequested(std::string& name) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.discoverMx);
    if (s.discoverReq != 1) return false;
    s.discoverReq = 0;
    name = s.discoverName;
    s.discoverName.clear();
    return true;
}

void start(void (*logSink)(const char*)) {
    State& s = S();
    if (s.sampler.joinable()) return;
    s.logSink = logSink;
    s.stopping = false;
    s.sampler = std::thread(sampleLoop);
}
void stop() {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.wakeMx);
        s.stopping = true;
    }
    s.wake.notify_all();
    if (s.sampler.joinable()) s.sampler.join();
}

}
