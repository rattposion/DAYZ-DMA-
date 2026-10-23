#pragma once
#include <cstdint>
#include <string>

namespace tel {

uint64_t nowUs();

enum Timer : int {
    T_CALL,
    T_READ1,
    T_DEVICE,
    T_XLATE,
    T_VIRT,
    T_LOCK,
    T_REFRESH,
    T_SCAN,
    T_SCANGAP,
    T_CAM,
    T_PAINT,
    T_FRAME,
    T_SNAPAGE,
    T_PRESENT,
    T_COUNT
};

enum Counter : int {
    C_CALLS, C_SINGLES, C_REQS, C_REQS_FAIL, C_BYTES, C_BYTES_OK,
    C_SCATTERS,
    C_PAGES, C_PAGES_FAIL,
    C_PTE_HIT, C_PTE_MISS, C_PTE_BAD,
    C_VIRT_FB, C_VIRT_FB_FAIL,
    C_LOCK_WAITS,
    C_STALLS,
    C_FRAMES, C_SCANS, C_SCANS_BAD, C_PTE_FLUSH, C_REFRESHES,
    C_COUNT
};

void time(Timer t, uint64_t us);
void count(Counter c, uint64_t n = 1);

struct CallInfo {
    bool single = false;
    uint32_t reqs = 0, reqsOk = 0;
    uint64_t bytes = 0, bytesOk = 0;
    uint64_t totalUs = 0, lockUs = 0;
};
void call(const CallInfo& c);

struct Suppress {
    Suppress();
    ~Suppress();
};

void requestDiscover(const std::string& name = "");
bool discoverRequested(std::string& name);

void start(void (*logSink)(const char*) = nullptr);
void stop();

}
