#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <exception>
#include <map>
#include <share.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clock.h"
#ifndef ICKZA_NO_DIAG
#include "discover.h"
#endif
#include "gpu_canvas.h"
#include "esp_launcher.h"
#include "image.h"
#include "loot.h"
#include "mapdl.h"
#include "process.h"
#include "radar.h"
#include "rtti.h"
#include "speedtest.h"
#include "telemetry.h"
#include "ui.h"
#include "version.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr COLORREF kKey = RGB(0, 0, 0);

struct Vec3 {
    float x, y, z;
};

enum DzJoint { DJ_Head, DJ_Neck, DJ_Spine, DJ_Pelvis, DJ_RSh, DJ_REl, DJ_RHand, DJ_LSh, DJ_LEl, DJ_LHand,
               DJ_RHip, DJ_RKnee, DJ_RFoot, DJ_LHip, DJ_LKnee, DJ_LFoot, DJ_Count };

struct Ent {
    Vec3 pos, feet;
    std::string name;
};

struct Skeleton {
    POINT pt[DJ_Count];
    bool ok[DJ_Count];
    int headRadius;
};

struct StickJoint {
    float nx, ny;
};
constexpr StickJoint kStick[] = {
    {0.50f, 0.00f},
    {0.50f, 0.18f},
    {0.30f, 0.20f},
    {0.27f, 0.38f},
    {0.25f, 0.56f},
    {0.70f, 0.20f},
    {0.73f, 0.38f},
    {0.75f, 0.56f},
    {0.50f, 0.54f},
    {0.41f, 0.56f},
    {0.42f, 0.78f},
    {0.43f, 0.98f},
    {0.42f, 1.00f},
    {0.59f, 0.56f},
    {0.58f, 0.78f},
    {0.57f, 0.98f},
    {0.58f, 1.00f},
};
constexpr std::pair<int, int> kStickBones[] = {
    {0, 1}, {1, 8},
    {1, 2}, {2, 3}, {3, 4},
    {1, 5}, {5, 6}, {6, 7},
    {8, 9}, {9, 10}, {10, 11}, {11, 12},
    {8, 13}, {13, 14}, {14, 15}, {15, 16},
};

struct Box {
    float x, y, w, h;
    Ent e;
    float dist;
    std::string weapon;
    bool enemy;
    Skeleton skel;
    int kind = 0;
};

std::unique_ptr<Process> gProc;
EspSettings gSettings;
bool gDma = false;
bool gFull = false;
bool gForceGdi = false;
std::unique_ptr<GpuRenderer> gGpu;
int gGpuFails = 0;
HWND gOverlay = nullptr, gGame = nullptr;
RECT gView{};
std::vector<Box> gBoxes;
int gFps = 0;
#ifndef ICKZA_NO_DIAG
std::atomic<double> gReadAvgMs{0}, gReadMaxMs{0};
double gPaintMs = 0;
std::atomic<double> gDzCamMs{0};
FILE* gLog = nullptr;
#endif
std::map<uint64_t, int> gDzKinds;
constexpr int LC_Other = loot::Other, LC_Weapon = loot::Gun, LC_Ammo = loot::Ammo, LC_Clothing = loot::Clothing, LC_Rare = loot::Rare;
std::map<uint64_t, int> gDzLootVts;
std::vector<std::string> gDzRare;
std::unordered_map<uint64_t, int> gDzEntKind;
std::unordered_map<uint64_t, Vec3> gDzEntPos;
std::unordered_map<uint64_t, uint64_t> gDzEntVs;
std::unordered_map<uint64_t, Vec3> gDzEntVel;
std::unordered_map<uint64_t, uint64_t> gDzEntPosUs;
std::unordered_map<uint64_t, std::pair<float, float>> gDzEntFwd;
std::unordered_map<uint64_t, std::string> gDzEntName;
std::unordered_map<uint64_t, uint64_t> gDzLootVs;
std::unordered_map<uint64_t, Vec3> gDzLootPos;
std::unordered_map<uint64_t, std::string> gDzLootName;
std::unordered_map<uint64_t, int> gDzLootCat;
std::unordered_map<uint64_t, std::string> gDzEntHeld, gDzItemName;
ULONGLONG gDzHeldTick = 0;
std::unordered_map<uint32_t, std::string> gDzRoster;
std::shared_ptr<const std::vector<std::string>> gDzOnline;
std::unordered_map<uint64_t, uint32_t> gDzEntNetId;
ULONGLONG gDzRosterTick = 0;
std::string gDzServer;
std::mutex gDzServerMx;
ULONGLONG gDzServerTick = 0;
ULONGLONG gDzFlushTick = 0, gDzPosSweepTick = 0, gDzLootSweepTick = 0;
uint64_t gDzWorld = 0;
ULONGLONG gDzWorldTick = 0;
std::string gDzMeName;
ULONGLONG gDzMeNameTick = 0;
std::string gDzMeNameDraw;
std::string gDzMeHeldDraw;
std::shared_ptr<const std::vector<std::string>> gDzOnlineDraw;
struct DzBoneChain {
    uint64_t anim = 0, arr = 0;
};
std::unordered_map<uint64_t, DzBoneChain> gDzEntChain;
std::unordered_map<uint64_t, std::array<Vec3, DJ_Count>> gDzEntJointOfs;
std::unordered_map<uint64_t, uint64_t> gDzEntReadUs, gDzEntBoneUs;
std::unordered_map<uint64_t, bool> gDzEntDead;
Vec3 gDzOmega{}, gDzVel{};

struct DzLootItem {
    Vec3 pos;
    std::string name;
    int cat = LC_Other;
};
struct DzSnap {
    uint64_t cam = 0;
    uint64_t tUs = 0;
    bool haveMe = false;
    Vec3 me{};
    std::string meName;
    std::string meHeld;
    struct E {
        int kind;
        uint64_t ptr = 0;
        Vec3 feet;
        float fwdX = 0, fwdZ = 0;
        Vec3 vel{};
        uint64_t posUs = 0;
        std::string name;
        std::string held;
        bool hasBones = false;
        Vec3 joints[DJ_Count];
    };
    std::vector<E> ents;
    std::shared_ptr<const std::vector<DzLootItem>> loot;
    std::shared_ptr<const std::vector<std::string>> online;
    size_t slots = 0, targets = 0, lootSlots = 0;
    int dead = 0;
#ifndef ICKZA_NO_DIAG
    std::vector<Vec3> dbgBones;
    int dbgKind = 0;
#endif
};
std::mutex gDzSnapMx;
DzSnap gDzSnap;
std::thread gDzReadThread;
constexpr ULONGLONG kLootPassMs = 100;
constexpr size_t kLootNamesPerPass = 256;
std::mutex gDzLootMx;
std::shared_ptr<const std::vector<DzLootItem>> gDzLootList;
size_t gDzLootSlotsPub = 0;
std::atomic<uint64_t> gDzWorldPub{0};
std::thread gDzLootThread;
void dzLootPass(uint64_t world);
std::atomic<bool> gDzReadRun{false};
std::atomic<bool> gDzGone{false};
#ifndef ICKZA_NO_DIAG
struct DbgBone {
    POINT pt;
    int idx;
};
std::vector<DbgBone> gDzDbgBones;
#endif
struct LootPt {
    POINT pt;
    float dist;
    std::string name;
    int cat;
};
struct RadarPt {
    float x, z;
    int kind;
    const std::string* name;
    float fwdX = 0, fwdZ = 0;
    const std::string* held = nullptr;
};
DzSnap gDzRadarSnap;
struct DzRadar {
    bool ok = false;
    Vec3 me{}, fwd{};
    std::vector<RadarPt> pts;
} gDzRadar;
std::vector<LootPt> gDzLootPts;

#ifndef ICKZA_NO_DIAG
void logLine(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::puts(line);
    if (!gLog) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(gLog, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
    std::fflush(gLog);
}

void openLog() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    p = p.substr(0, p.find_last_of("\\/") + 1) + "Ickza.log";
    gLog = _fsopen(p.c_str(), "w", _SH_DENYNO);
    if (!gLog) std::printf("[!] could not write %s\n", p.c_str());
}

void runDiscoverSweep(const std::string& nameArg);
const char* const kQuickSweep = "\x01quick";

std::string moduleOffset(const void* a) {
    char buf[400];
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* slash = std::strrchr(name, '\\');
        std::snprintf(buf, sizeof buf, "%s+0x%llX", slash ? slash + 1 : name,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(m)));
    } else {
        std::snprintf(buf, sizeof buf, "%p", a);
    }
    return buf;
}

void logCrash(const char* what, const EXCEPTION_POINTERS* ep) {
    static std::atomic<bool> once{false};
    if (once.exchange(true) || !gLog) return;
    std::fprintf(gLog, "CRASH: %s on thread %lu\n", what, GetCurrentThreadId());
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        std::fprintf(gLog, "CRASH: code 0x%08lX at %s", er->ExceptionCode, moduleOffset(er->ExceptionAddress).c_str());
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            std::fprintf(gLog, ", %s address 0x%llX", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                         static_cast<unsigned long long>(er->ExceptionInformation[1]));
        std::fputc('\n', gLog);
    }
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) std::fprintf(gLog, "CRASH:   #%u %s\n", unsigned(i), moduleOffset(frames[i]).c_str());
    std::fflush(gLog);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    logCrash("unhandled exception", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminateHandler() {
    const char* what = "std::terminate (uncaught C++ exception or noexcept violation)";
    char buf[300];
    if (const std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::snprintf(buf, sizeof buf, "uncaught C++ exception: %s", e.what());
            what = buf;
        } catch (...) {
        }
    }
    logCrash(what, nullptr);
    std::abort();
}

void installCrashHandlers() {
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, [](int) { logCrash("abort() called", nullptr); });
    _set_purecall_handler([] { logCrash("pure virtual call", nullptr); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { logCrash("invalid CRT parameter", nullptr); });
}

void logWindow(const char* what, HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    logLine("[log] %s: %ld,%ld %ldx%ld on %s, visible=%d topmost=%d dpi=%u foreground=%d", what, r.left, r.top, r.right - r.left,
         r.bottom - r.top, mi.szDevice, IsWindowVisible(hwnd) ? 1 : 0, (GetWindowLongA(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0,
         GetDpiForWindow(hwnd), GetForegroundWindow() == hwnd ? 1 : 0);
}
#endif

HWND findGameWindow(DWORD pid) {
    struct Ctx {
        DWORD pid;
        HWND hwnd;
    } ctx{pid, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(hwnd, &p);
            if (p != c->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
            c->hwnd = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

bool placeOverlay() {
    if (!gGame || !IsWindow(gGame)) gGame = findGameWindow(gProc->pid());
    if (!gGame || IsIconic(gGame) || GetForegroundWindow() != gGame) {
        ShowWindow(gOverlay, SW_HIDE);
        return false;
    }
    RECT rc;
    GetClientRect(gGame, &rc);
    POINT tl{0, 0};
    ClientToScreen(gGame, &tl);
    SetWindowPos(gOverlay, HWND_TOPMOST, tl.x, tl.y, rc.right, rc.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    gView = {0, 0, rc.right, rc.bottom};
    return true;
}

bool placeDmaView(const float* proj) {
    RECT rc;
    GetClientRect(gOverlay, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    float aspect = 16.f / 9;
    if (proj && proj[0] > 0 && proj[5] > 0) aspect = proj[5] / proj[0];
    int vw = rc.right, vh = int(vw / aspect);
    if (vh > rc.bottom) {
        vh = rc.bottom;
        vw = int(vh * aspect);
    }
    const int ox = (rc.right - vw) / 2, oy = (rc.bottom - vh) / 2;
    gView = {ox, oy, ox + vw, oy + vh};
    return true;
}

bool updateFrame(const char*& stage, int& count);

bool update() {
    static ULONGLONG lastAliveCheck = 0;
#ifndef ICKZA_NO_DIAG
    static ULONGLONG lastStatus = 0;
    static const char* lastStage = "";
    static int frames = 0;
#endif
    const ULONGLONG now = GetTickCount64();
    if (now - lastAliveCheck >= 500) {
        lastAliveCheck = now;
        static int aliveMiss = 0;
        if (gProc->alive()) {
            aliveMiss = 0;
        } else if (++aliveMiss >= 4) {
            logLine("[log] game process gone on the target (%d checks)", aliveMiss);
            aliveMiss = 0;
            return false;
        }
    }
    {
        static bool upWas = false, dnWas = false;
        const bool up = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0, dn = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
        if ((up && !upWas) || (dn && !dnWas)) {
            gSettings.predictMs = std::max(0, std::min(60, gSettings.predictMs + (up && !upWas ? 1 : -1)));
            logLine("[log] predict_ms = %d", gSettings.predictMs);
            saveSettings(gSettings);
        }
        upWas = up;
        dnWas = dn;
    }
    const char* stage = "";
    int count = 0;
#ifndef ICKZA_NO_DIAG
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t0, t1;
#endif
#ifndef ICKZA_NO_DIAG
    QueryPerformanceCounter(&t0);
#endif
    updateFrame(stage, count);
#ifndef ICKZA_NO_DIAG
    QueryPerformanceCounter(&t1);
    ++frames;
    {
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        tel::time(tel::T_SCAN, uint64_t(ms * 1000.0));
        tel::count(tel::C_SCANS);
        if (std::strcmp(stage, "ok") != 0 && std::strcmp(stage, "") != 0) tel::count(tel::C_SCANS_BAD);
    }
#endif
    gProc->maintain();

    static ULONGLONG lastFps = now;
    static int fpsFrames = 0;
    ++fpsFrames;
#ifndef ICKZA_NO_DIAG
    static double readSum = 0, readMax = 0;
    const double readMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
    readSum += readMs;
    readMax = std::max(readMax, readMs);
#endif
    if (now - lastFps >= 1000) {
        gFps = int(fpsFrames * 1000 / (now - lastFps));
#ifndef ICKZA_NO_DIAG
        gReadAvgMs = readSum / fpsFrames;
        gReadMaxMs = readMax;
        readSum = readMax = 0;
#endif
        lastFps = now;
        fpsFrames = 0;
    }

#ifndef ICKZA_NO_DIAG
    if (std::strcmp(stage, lastStage) != 0) {
        logLine("[log] frame: %s", stage);
        lastStage = stage;
    }
    if (now - lastStatus >= 2000) {
        logLine("[log] status: %d frames/2s (%d fps, reads %.1f ms avg / %.1f max, paint %.1f ms), players=%d boxes=%zu view=%ld,%ld %ldx%ld",
                frames, gFps, gReadAvgMs.load(), gReadMaxMs.load(), gPaintMs, count, gBoxes.size(), gView.left, gView.top, gView.right - gView.left,
                gView.bottom - gView.top);
        if (gDma) logWindow("window", gOverlay);
        lastStatus = now;
        frames = 0;
    }
#endif
    return true;
}

namespace dz {
constexpr uint32_t kWorld = 0x1832B80;
constexpr uint32_t kCamera = 0x1B0;
constexpr uint32_t kNear = 0xF70, kFar = 0x10B8;
constexpr uint32_t kLoot = 0x2218;
constexpr size_t kMaxLoot = 8192;
constexpr uint32_t kItemCfg = 0x130;
constexpr uint32_t kCfgName = 0x98;
constexpr uint32_t kNameText = 0x10;
constexpr uint32_t kMeHub = 0x30, kMeA = 0x8, kMeB = 0x3E8, kMeSlot = 0xC0;
constexpr uint32_t kNcWorldName = 0x38;
constexpr uint32_t kNcServerName = 0x308;
constexpr uint32_t kHeldA = 0x5D8;
constexpr uint32_t kHeldB = 0x190;
constexpr ULONGLONG kHeldMs = 500;
constexpr uint32_t kNetClient = 0x4A8C4D0;
constexpr uint32_t kNcRoster = 0x18;
constexpr uint32_t kIdName = 0xF8;
constexpr uint32_t kIdNetId = 0x30, kEntNetId = 0x664;
constexpr int kMaxRoster = 256;
constexpr ULONGLONG kRosterMs = 2000;
constexpr uint32_t kLocalLink = 0x29B0, kLocalDeref = 0x8, kLocalAdj = 0xA8;
constexpr uint32_t kFlags = 0x150;
constexpr uint32_t kIsDead = 0x156;
constexpr uint32_t kVisualState = 0x158;
constexpr uint32_t kVsPos = 0x2C;
constexpr uint32_t kCamBasis = 0x8;
constexpr uint32_t kCamProjD1 = 0xD0, kCamProjD2 = 0xDC;
constexpr size_t kCamBytes = 0xE8, kMaxEnts = 512;
constexpr float kHeight = 1.8f;

constexpr uint32_t kSkelPlayer = 0xB98;
constexpr uint32_t kSkelInfected = 0x620;
constexpr uint32_t kAnimClass = 0x188;
constexpr uint32_t kMatrixArray = 0x38;
constexpr uint32_t kBoneTrans = 0x54;
constexpr uint32_t kBoneStride = 0x30;
constexpr uint32_t kVsTransform = 0x8;
constexpr int kMaxBones = 150;

constexpr int kBoneInfected[DJ_Count] = {64, 29, 26, 0, 31, 35, 39, 65, 69, 73, 13, 16, 18, 19, 22, 24};
constexpr int kBonePlayer[DJ_Count] = {90, 52, 48, 0, 57, 61, 63, 127, 131, 133, 25, 35, 38, 28, 42, 45};
constexpr std::pair<int, int> kDzSkelBones[] = {
    {DJ_Head, DJ_Neck},   {DJ_Neck, DJ_Spine},  {DJ_Spine, DJ_Pelvis},
    {DJ_Neck, DJ_RSh},    {DJ_RSh, DJ_REl},     {DJ_REl, DJ_RHand},
    {DJ_Neck, DJ_LSh},    {DJ_LSh, DJ_LEl},     {DJ_LEl, DJ_LHand},
    {DJ_Pelvis, DJ_RHip}, {DJ_RHip, DJ_RKnee},  {DJ_RKnee, DJ_RFoot},
    {DJ_Pelvis, DJ_LHip}, {DJ_LHip, DJ_LKnee},  {DJ_LKnee, DJ_LFoot},
};
constexpr float kNearM = 50.f;
constexpr float kMidM = 150.f;
constexpr uint64_t kMidUs = 8000;
constexpr uint64_t kFarVisUs = 25000;
constexpr uint64_t kOffUs = 50000;
constexpr uint64_t kOutUs = 100000;
constexpr uint64_t kBoneMidUs = 33000;
constexpr ULONGLONG kKindFlushMs = 2000;

struct Cam {
    Vec3 right, up, fwd, pos;
    float px, py;
};

bool orthonormal(const float* r) {
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(r[i])) return false;
    for (int i = 0; i < 3; ++i) {
        const float* a = r + 3 * i;
        if (std::fabs(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] - 1) > 0.02f) return false;
        for (int j = i + 1; j < 3; ++j) {
            const float* b = r + 3 * j;
            if (std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) > 0.02f) return false;
        }
    }
    return true;
}

bool parseCam(const uint8_t* b, Cam& c) {
    static uint32_t basis = kCamBasis;
#ifndef ICKZA_NO_DIAG
    static bool logged = false, warnedProj = false;
#endif
    auto fits = [&](uint32_t o) { return o + 48 <= kCamBytes && orthonormal(reinterpret_cast<const float*>(b + o)); };
    if (!fits(basis)) {
        uint32_t found = 0;
        for (uint32_t o = 0; o + 48 <= kCamBytes && !found; o += 4)
            if (fits(o)) found = o;
        if (!found) return false;
        logLine("[!] DayZ camera basis is not at +0x%X; using +0x%X", basis, found);
        basis = found;
    }
    std::memcpy(&c.right, b + basis, 12);
    std::memcpy(&c.up, b + basis + 12, 12);
    std::memcpy(&c.fwd, b + basis + 24, 12);
    std::memcpy(&c.pos, b + basis + 36, 12);
    std::memcpy(&c.px, b + kCamProjD1, 4);
    std::memcpy(&c.py, b + kCamProjD2 + 4, 4);
    if (!(c.px > 0.05f && c.px < 20 && c.py > 0.05f && c.py < 20)) {
#ifndef ICKZA_NO_DIAG
        if (!warnedProj) logLine("[!] DayZ camera projection unreadable (%.3f, %.3f); assuming 90 deg 16:9", c.px, c.py);
        warnedProj = true;
#endif
        c.px = 1;
        c.py = 9.f / 16;
    }
#ifndef ICKZA_NO_DIAG
    if (!logged) {
        logLine("[log] DayZ camera: basis +0x%X, pos %.1f %.1f %.1f, tan(fov/2) %.3f x %.3f", basis, c.pos.x, c.pos.y, c.pos.z, c.px, c.py);
        logged = true;
    }
#endif
    return true;
}

bool toScreen(const Cam& c, const Vec3& p, float w, float h, float& sx, float& sy) {
    const Vec3 d{p.x - c.pos.x, p.y - c.pos.y, p.z - c.pos.z};
    const float x = d.x * c.right.x + d.y * c.right.y + d.z * c.right.z;
    const float y = d.x * c.up.x + d.y * c.up.y + d.z * c.up.z;
    const float z = d.x * c.fwd.x + d.y * c.fwd.y + d.z * c.fwd.z;
    if (z < 0.1f) return false;
    sx = w / 2 * (1 + x / c.px / z);
    sy = h / 2 * (1 - y / c.py / z);
    return true;
}

bool heapPtr(uint64_t v) { return v >= 0x10000 && v < 0x7FFFFFFF0000ull && v % 8 == 0; }

inline Vec3 vadd(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 vsub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 vscale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float vdot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 vcross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float vlen(const Vec3& a) { return std::sqrt(vdot(a, a)); }
inline Vec3 vlerp(const Vec3& a, const Vec3& b, float t) { return vadd(vscale(a, 1 - t), vscale(b, t)); }
inline Vec3 rodrigues(const Vec3& v, const Vec3& k, float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    return vadd(vadd(vscale(v, c), vscale(vcross(k, v), s)), vscale(k, vdot(k, v) * (1 - c)));
}

}

static_assert(dz::kVsTransform + 36 == dz::kVsPos, "VisualState position is the transform's translation");

std::string exeDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p = path;
    return p.substr(0, p.find_last_of("\\/") + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void dzLoadRare() {
    gDzRare.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, (exeDir() + "dayz_rare.txt").c_str(), "r") || !f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        s = s.substr(0, s.find('#'));
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        if (a < s.size()) gDzRare.push_back(lower(s.substr(a)));
    }
    std::fclose(f);
    logLine("[*] dayz_rare.txt: %zu rare item pattern(s)", gDzRare.size());
}

bool dzIsRare(const std::string& name) {
    if (name.empty()) return false;
    const std::string n = lower(name);
    for (const std::string& r : gDzRare) {
        if (!r.empty() && r.back() == '*') {
            if (n.compare(0, r.size() - 1, r, 0, r.size() - 1) == 0) return true;
        } else if (n == r) {
            return true;
        }
    }
    return false;
}

int dzLootCategory(int vtCat, const std::string& name) { return dzIsRare(name) ? LC_Rare : loot::classify(vtCat, name); }

std::string dzPretty(int kind, const std::string& raw) {
    static const std::pair<const char*, const char*> kAnimals[] = {
        {"CervusElaphus", "Deer"},       {"CapreolusCapreolus", "Roe deer"}, {"SusScrofa", "Boar"},
        {"SusDomesticus", "Pig"},        {"BosTaurus", "Cow"},               {"OvisAries", "Sheep"},
        {"CapraHircus", "Goat"},         {"GallusGallus", "Chicken"},        {"CanisLupus", "Wolf"},
        {"UrsusArctos", "Bear"},         {"UrsusMaritimus", "Polar bear"},   {"LepusEuropaeus", "Hare"},
        {"VulpesVulpes", "Fox"},         {"RangiferTarandus", "Reindeer"},
    };
    static const std::pair<const char*, const char*> kVehicles[] = {
        {"OffroadHatchback", "Ada 4x4"}, {"CivilianSedan", "Olga 24"}, {"Hatchback_02", "Gunter 2"}, {"Sedan_02", "Sarka 120"},
        {"Truck_01", "M3S truck"},       {"Offroad_02", "Humvee"},     {"Boat_", "Boat"},
    };
    if (kind == 3) {
        for (const auto& [k, v] : kAnimals)
            if (raw.find(k) != std::string::npos) return v;
        return raw.rfind("Animal_", 0) == 0 ? raw.substr(7) : raw;
    }
    if (kind == 4)
        for (const auto& [k, v] : kVehicles)
            if (raw.find(k) != std::string::npos) return v;
    return raw;
}

std::vector<std::string> dzTypeNames(const std::vector<uint64_t>& objs) {
    std::vector<uint64_t> cfg(objs.size(), 0), str(objs.size(), 0);
    std::vector<Process::ReadReq> rq;
    for (size_t i = 0; i < objs.size(); ++i) rq.push_back({uintptr_t(objs[i] + dz::kItemCfg), &cfg[i], 8});
    gProc->readMany(rq);
    rq.clear();
    for (size_t i = 0; i < objs.size(); ++i) rq.push_back({uintptr_t(dz::heapPtr(cfg[i]) ? cfg[i] + dz::kCfgName : 0), &str[i], 8});
    gProc->readMany(rq);
    constexpr size_t kN = 48;
    std::vector<char> txt(objs.size() * kN, 0);
    rq.clear();
    for (size_t i = 0; i < objs.size(); ++i) rq.push_back({uintptr_t(dz::heapPtr(str[i]) ? str[i] + dz::kNameText : 0), &txt[i * kN], kN});
    gProc->readMany(rq);
    std::vector<std::string> out(objs.size());
    for (size_t i = 0; i < objs.size(); ++i) {
        if (!rq[i].ok) continue;
        const char* t = &txt[i * kN];
        size_t n = 0;
        bool ident = std::isalpha(static_cast<unsigned char>(t[0])) != 0;
        while (ident && n < kN && t[n]) {
            const unsigned char c = static_cast<unsigned char>(t[n]);
            ident = std::isalnum(c) || c == '_';
            ++n;
        }
        if (ident && n >= 3 && n < kN) out[i] = std::string(t, n);
    }
    return out;
}

float dzMapSize(const std::string& map) {
    static const std::pair<const char*, float> kMaps[] = {
        {"chernarusplus", 15360.f}, {"livonia", 12800.f}, {"enoch", 12800.f},   {"namalsk", 12800.f}, {"sakhal", 15360.f}, {"deerisle", 16384.f},
        {"esseker", 12800.f},       {"takistan", 12800.f}, {"banov", 15360.f},  {"alteria", 8192.f},  {"deadfall", 10000.f}, {"lux", 15360.f},
    };
    const std::string m = lower(map);
    if (const float s = mapdl::savedSize(m); s > 0) return s;
    for (const auto& [k, v] : kMaps)
        if (m == k) return v;
    if (const mapdl::Entry* e = mapdl::find(m)) return float(e->size);
    return 15360;
}

bool dzMapImageExists(const std::string& base) {
    for (const char* ext : {".png", ".jpg"}) {
        const DWORD a = GetFileAttributesA((base + ext).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return true;
    }
    return false;
}
std::string dzMapStyleBase(const std::string& id, bool topo) { return exeDir() + "maps\\" + id + (topo ? "_topo" : ""); }
std::string dzMapBase(const std::string& id) {
    const std::string want = dzMapStyleBase(id, gSettings.dzMapTopo);
    if (dzMapImageExists(want)) return want;
    const std::string other = dzMapStyleBase(id, !gSettings.dzMapTopo);
    return dzMapImageExists(other) ? other : want;
}

int gMapsGen = 0;
std::string gMapTried;

const std::vector<radar::Config::MapItem>& radarMapList() {
    static std::vector<radar::Config::MapItem> list;
    static int gen = -1;
    static int topo = -1;
    static std::string cur;
    if (gen == gMapsGen && topo == int(gSettings.dzMapTopo) && cur == gSettings.dzMap) return list;
    gen = gMapsGen;
    topo = int(gSettings.dzMapTopo);
    cur = gSettings.dzMap;
    list.clear();
    std::vector<std::string> ids;
    auto add = [&](const std::string& id) {
        if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    };
    auto endsWith = [](const std::string& s, const char* suf) {
        const size_t n = std::strlen(suf);
        return s.size() > n && s.compare(s.size() - n, n, suf) == 0;
    };
    for (const char* ext : {"\\*.png", "\\*.jpg"}) {
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((exeDir() + "maps" + ext).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string n = fd.cFileName;
            const size_t dot = n.find_last_of('.');
            if (dot != std::string::npos) n.resize(dot);
            if (endsWith(n, "_names")) continue;
            if (endsWith(n, "_topo")) n.resize(n.size() - 5);
            add(lower(n));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const mapdl::Entry& e : mapdl::catalog()) add(e.id);
    add(gSettings.dzMap);
    for (const std::string& id : ids)
        list.push_back({id, mapdl::label(id), dzMapImageExists(dzMapStyleBase(id, gSettings.dzMapTopo)), mapdl::official(id)});
    auto rank = [](const radar::Config::MapItem& m) {
        const auto& c = mapdl::catalog();
        for (size_t i = 0; i < c.size(); ++i)
            if (m.official && m.id == c[i].id) return int(i);
        return 1000;
    };
    std::sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
        const int ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : lower(a.label) < lower(b.label);
    });
    return list;
}

std::string dzMapIdForWorld(const std::string& worldName) {
    std::string w = lower(worldName);
    const size_t slash = w.find_last_of("/\\");
    if (slash != std::string::npos) w = w.substr(slash + 1);
    if (w.empty()) return "";
    auto resolve = [](const std::string& n) -> std::string {
        if (n.empty()) return "";
        if (n == "chernarus") return "chernarusplus";
        if (n == "enoch") return "livonia";
        if (n == "takistanplus") return "takistan";
        if (mapdl::find(n) || dzMapImageExists(dzMapStyleBase(n, false)) || dzMapImageExists(dzMapStyleBase(n, true))) return n;
        return "";
    };
    if (std::string id = resolve(w); !id.empty()) return id;
    const size_t dot = w.find_last_of('.');
    if (dot != std::string::npos) return resolve(w.substr(dot + 1));
    return "";
}

void dzMapDownloads() {
    static int gen = 0;
    static ULONGLONG checked = 0;
    static const bool dirSet = [] {
        mapdl::setDir(exeDir() + "maps\\");
        return true;
    }();
    (void)dirSet;
    const mapdl::Status st = mapdl::status();
    if (st.generation != gen) {
        gen = st.generation;
        ++gMapsGen;
        radar::reloadMaps();
        if (gGpu) gGpu->releaseImages();
    }
    const std::string key = gSettings.dzMap + (gSettings.dzMapTopo ? "|topo" : "|sat");
    const ULONGLONG now = GetTickCount64();
    if (st.busy || key == gMapTried || (now - checked < 1000)) return;
    checked = now;
    if (gSettings.dzMap.empty() || dzMapImageExists(dzMapStyleBase(gSettings.dzMap, gSettings.dzMapTopo))) return;
    gMapTried = key;
    mapdl::start(gSettings.dzMap, gSettings.dzMapTopo, gSettings.dzMapHigh);
}
void dzMapDownloadNow(const std::string& id) {
    gMapTried = id + (gSettings.dzMapTopo ? "|topo" : "|sat");
    mapdl::start(id, gSettings.dzMapTopo, gSettings.dzMapHigh);
}

std::mutex gDzMapMx;
std::string gDzMapDetected;
std::string gDzMapSeen;
std::atomic<bool> gDzMapRedetect{false};

void dzDetectMap(uint64_t world) {
    if (!gProc || !world) return;
    static ULONGLONG tick = 0;
    static std::string last;
    const ULONGLONG now = GetTickCount64();
    if (gDzMapRedetect.exchange(false)) {
        last.clear();
        tick = 0;
    }
    if (tick && now - tick < 5000) return;
    tick = now;

    uint64_t nc = 0, str = 0;
    uint8_t raw[64] = {};
    if (!gProc->read(uintptr_t(gProc->module().base + dz::kNetClient), nc) || !dz::heapPtr(nc) ||
        !gProc->read(uintptr_t(nc + dz::kNcWorldName), str) || !dz::heapPtr(str) ||
        !gProc->read(uintptr_t(str + dz::kNameText), raw, sizeof raw - 1))
        return;
    std::string text;
    for (size_t i = 0; i < sizeof raw && raw[i]; ++i) {
        if (raw[i] < 0x20 || raw[i] >= 0x7F) return;
        text += char(raw[i]);
    }

    const std::string id = text.empty() ? std::string() : dzMapIdForWorld(text);
    if (id.empty()) return;
#ifndef ICKZA_NO_DIAG
    static std::string lastText;
    if (text != lastText) {
        lastText = text;
        logLine("[+] radar: game world '%s' -> map %s", text.c_str(), id.c_str());
    }
#endif
    std::lock_guard<std::mutex> lk(gDzMapMx);
    gDzMapSeen = id;
    if (id != last) {
        last = id;
        gDzMapDetected = id;
    }
}

struct DzCamShare {
    dz::Cam c{};
    uint64_t tUs = 0;
    uint64_t tChangeUs = 0;
    Vec3 omega{}, vel{};
};
std::mutex gDzCamMx;
DzCamShare gDzCamShare;

bool dzMaybeVisible(const dz::Cam& c, const Vec3& feet) {
    constexpr float kR = 1.5f;
    constexpr float kMargin = 0.35f;
    const Vec3 d{feet.x - c.pos.x, feet.y + 0.9f - c.pos.y, feet.z - c.pos.z};
    const float x = dz::vdot(d, c.right), y = dz::vdot(d, c.up), z = dz::vdot(d, c.fwd);
    if (x * x + y * y + z * z < 25.f) return true;
    if (z < -kR) return false;
    const float tx = std::tan(std::min(std::atan(c.px) + kMargin, 1.45f));
    const float ty = std::tan(std::min(std::atan(c.py) + kMargin, 1.45f));
    return std::fabs(x) <= z * tx + kR * std::sqrt(1 + tx * tx) && std::fabs(y) <= z * ty + kR * std::sqrt(1 + ty * ty);
}

Vec3 boneToWorld(const float tf[12], const Vec3& l) {
    return {tf[9] + l.x * tf[0] + l.y * tf[3] + l.z * tf[6], tf[10] + l.x * tf[1] + l.y * tf[4] + l.z * tf[7],
            tf[11] + l.x * tf[2] + l.y * tf[5] + l.z * tf[8]};
}

bool placeJoints(DzSnap::E& e, const float tf[12], const Vec3* local, const Process::ReadReq* rq) {
    std::array<Vec3, DJ_Count> j;
    bool any = false;
    for (int b = 0; b < DJ_Count; ++b) {
        if (!rq[b].ok) { j[b] = {NAN, NAN, NAN}; continue; }
        j[b] = boneToWorld(tf, local[b]);
        const float dx = j[b].x - tf[9], dy = j[b].y - tf[10], dz2 = j[b].z - tf[11];
        if (!(dx * dx + dy * dy + dz2 * dz2 < 5.f * 5.f)) return false;
        any = true;
    }
    if (!any) return false;
    std::array<Vec3, DJ_Count> ofs;
    for (int b = 0; b < DJ_Count; ++b) {
        e.joints[b] = j[b];
        ofs[b] = {j[b].x - tf[9], j[b].y - tf[10], j[b].z - tf[11]};
    }
    e.hasBones = true;
    gDzEntJointOfs[e.ptr] = ofs;
    return true;
}

struct BoneJob {
    uint64_t ent;
    int kind;
    size_t entIdx;
    float tf[12] = {};
    uint64_t skel = 0, anim = 0, arr = 0;
};

void readBonesBatch(std::vector<BoneJob>& jobs, DzSnap& out) {
    if (jobs.empty()) return;
    std::vector<Process::ReadReq> reqs;

    for (auto& j : jobs) reqs.push_back({uintptr_t(j.ent + (j.kind == 1 ? dz::kSkelPlayer : dz::kSkelInfected)), &j.skel, 8});
    gProc->readMany(reqs);

    reqs.clear();
    for (auto& j : jobs) reqs.push_back({uintptr_t((dz::heapPtr(j.skel) ? j.skel : 0) + dz::kAnimClass), &j.anim, 8});
    gProc->readMany(reqs);

    reqs.clear();
    for (auto& j : jobs) reqs.push_back({uintptr_t((dz::heapPtr(j.anim) ? j.anim : 0) + dz::kMatrixArray), &j.arr, 8});
    gProc->readMany(reqs);

    reqs.clear();
    std::vector<std::array<Vec3, DJ_Count>> locals(jobs.size());
    for (size_t k = 0; k < jobs.size(); ++k) {
        const int* tbl = (jobs[k].kind == 1) ? dz::kBonePlayer : dz::kBoneInfected;
        const uint64_t arr = dz::heapPtr(jobs[k].arr) ? jobs[k].arr : 0;
        for (int b = 0; b < DJ_Count; ++b)
            reqs.push_back({uintptr_t(arr + dz::kBoneTrans + uint32_t(tbl[b]) * dz::kBoneStride), &locals[k][b], 12});
    }
    gProc->readMany(reqs);

    for (size_t k = 0; k < jobs.size(); ++k) {
        if (!dz::heapPtr(jobs[k].arr)) continue;
        if (placeJoints(out.ents[jobs[k].entIdx], jobs[k].tf, locals[k].data(), &reqs[k * DJ_Count]))
            gDzEntChain[jobs[k].ent] = {jobs[k].anim, jobs[k].arr};
    }
}

#ifndef ICKZA_NO_DIAG
void readBonesWorld(uint64_t entity, int kind, uint64_t vs, std::vector<Vec3>& out) {
    out.clear();
    const uint32_t skelOff = (kind == 1) ? dz::kSkelPlayer : dz::kSkelInfected;
    uint64_t skeleton = 0, animClass = 0, matrixArray = 0;
    const bool okSkel = gProc->read(entity + skelOff, skeleton) && dz::heapPtr(skeleton);
    const bool okAnim = okSkel && gProc->read(skeleton + dz::kAnimClass, animClass) && dz::heapPtr(animClass);
    const bool okArr = okAnim && gProc->read(animClass + dz::kMatrixArray, matrixArray) && dz::heapPtr(matrixArray);
#ifndef ICKZA_NO_DIAG
    static ULONGLONG lastChainLog = 0;
    const ULONGLONG t = GetTickCount64();
    if (t - lastChainLog >= 2000) {
        lastChainLog = t;
        logLine("[log] bonechain kind %d: ent 0x%llX skel 0x%llX anim 0x%llX arr 0x%llX", kind,
                (unsigned long long)entity, (unsigned long long)skeleton, (unsigned long long)animClass, (unsigned long long)matrixArray);
    }
#endif
    if (!okArr) return;
    float tf[12] = {};
    if (!gProc->read(vs + dz::kVsTransform, tf)) return;
    const Vec3 r0{tf[0], tf[1], tf[2]}, r1{tf[3], tf[4], tf[5]}, r2{tf[6], tf[7], tf[8]}, T{tf[9], tf[10], tf[11]};
    std::vector<Vec3> local(dz::kMaxBones);
    std::vector<Process::ReadReq> reqs;
    for (int i = 0; i < dz::kMaxBones; ++i)
        reqs.push_back({uintptr_t(matrixArray + dz::kBoneTrans + uint32_t(i) * dz::kBoneStride), &local[i], 12});
    gProc->readMany(reqs);
    out.assign(dz::kMaxBones, Vec3{NAN, NAN, NAN});
    for (int i = 0; i < dz::kMaxBones; ++i) {
        if (!reqs[i].ok) continue;
        const Vec3& l = local[i];
        out[i] = {T.x + l.x * r0.x + l.y * r1.x + l.z * r2.x, T.y + l.x * r0.y + l.y * r1.y + l.z * r2.y,
                  T.z + l.x * r0.z + l.y * r1.z + l.z * r2.z};
    }
}
#endif

void scanDayz(DzSnap& out, const char*& stage) {
    const uintptr_t base = gProc->module().base;
    const ULONGLONG now = GetTickCount64();
    uint64_t world = gDzWorld;
    if (!world || now - gDzWorldTick >= 1000) {
        std::vector<Process::ReadReq> wreq = {{base + dz::kWorld, &world, 8}};
        gProc->readMany(wreq);
        stage = "World pointer unreadable or null (in game?)";
        if (!wreq[0].ok || !dz::heapPtr(world)) { gDzWorld = 0; return; }
        static uint64_t namesWorld = 0;
        if (world != namesWorld) {
            namesWorld = world;
            gDzMeName.clear();
            gDzRoster.clear();
            gDzOnline.reset();
            gDzEntNetId.clear();
            gDzServerTick = 0;
        }
        gDzWorld = world;
        gDzWorldTick = now;
    }
    dzDetectMap(world);
    std::vector<Process::ReadReq> reqs;

    uint64_t cam = 0, link = 0;
    uint64_t tables[2][2] = {};
    reqs = {{uintptr_t(world + dz::kCamera), &cam, 8},
            {uintptr_t(world + dz::kNear), tables[0], 16},
            {uintptr_t(world + dz::kFar), tables[1], 16},
            {uintptr_t(world + dz::kLocalLink), &link, 8}};
    gProc->readMany(reqs);
    stage = "camera pointer unreadable (still loading?)";
    if (!reqs[0].ok || !dz::heapPtr(cam)) { gDzWorld = 0; return; }
    out.cam = cam;

    std::vector<uint64_t> slots;
    std::vector<std::pair<size_t, size_t>> spans;
    for (auto& t : tables) {
        const uint64_t n = t[1] & 0xFFFFFFFF;
        if (!dz::heapPtr(t[0]) || n == 0 || n > dz::kMaxEnts) continue;
        spans.push_back({slots.size(), size_t(n)});
        slots.resize(slots.size() + n, 0);
    }
    uint64_t localInner = 0;
    reqs.clear();
    size_t ti = 0;
    for (auto& t : tables) {
        const uint64_t n = t[1] & 0xFFFFFFFF;
        if (!dz::heapPtr(t[0]) || n == 0 || n > dz::kMaxEnts) continue;
        reqs.push_back({uintptr_t(t[0]), &slots[spans[ti].first], spans[ti].second * 8});
        ++ti;
    }
    if (dz::heapPtr(link)) reqs.push_back({uintptr_t(link + dz::kLocalDeref), &localInner, 8});
    if (!reqs.empty()) gProc->readMany(reqs);
    const uint64_t local = dz::heapPtr(localInner) ? localInner - dz::kLocalAdj : 0;

    std::vector<uint64_t> ents;
    for (uint64_t s : slots)
        if (dz::heapPtr(s) && s != local) ents.push_back(s);
    if (local) ents.push_back(local);
    out.slots = ents.size();

    if (now - gDzFlushTick >= dz::kKindFlushMs) {
        gDzEntKind.clear();
        gDzEntChain.clear();
        gDzFlushTick = now;
    }
    const bool posSweep = now - gDzPosSweepTick >= 10000;
    if (posSweep) {
        gDzEntName.clear();
        gDzEntNetId.clear();
        gDzPosSweepTick = now;
    }

    std::vector<uint64_t> unknown;
    for (uint64_t e : ents)
        if (e != local && gDzEntKind.find(e) == gDzEntKind.end()) unknown.push_back(e);
    if (!unknown.empty()) {
        std::vector<uint64_t> vts(unknown.size(), 0);
        reqs.clear();
        for (size_t i = 0; i < unknown.size(); ++i) reqs.push_back({uintptr_t(unknown[i]), &vts[i], 8});
        gProc->readMany(reqs);
        for (size_t i = 0; i < unknown.size(); ++i) {
            if (!reqs[i].ok) continue;
            const auto k = gDzKinds.find(vts[i]);
            gDzEntKind[unknown[i]] = (k == gDzKinds.end()) ? 0 : k->second;
        }
    }

    struct Target {
        uint64_t ptr;
        int kind;
    };
    std::vector<Target> targets;
    for (uint64_t e : ents) {
        if (e == local) { targets.push_back({e, 0}); continue; }
        const auto it = gDzEntKind.find(e);
        if (it == gDzEntKind.end()) continue;
        const int k = it->second;
        if (k == 1 || (k == 2 && gSettings.dzInfected) || (k == 3 && gSettings.dzAnimals) || (k == 4 && gSettings.dzVehicles)) targets.push_back({e, k});
    }
    out.targets = targets.size();
    if (posSweep) {
        std::unordered_set<uint64_t> live;
        for (const Target& t : targets) live.insert(t.ptr);
        const auto prune = [&live](auto& m) {
            for (auto it = m.begin(); it != m.end();) it = live.count(it->first) ? std::next(it) : m.erase(it);
        };
        prune(gDzEntPos);
        prune(gDzEntVs);
        prune(gDzEntVel);
        prune(gDzEntPosUs);
        prune(gDzEntFwd);
        prune(gDzEntJointOfs);
        prune(gDzEntReadUs);
        prune(gDzEntBoneUs);
        prune(gDzEntDead);
    }

#ifndef ICKZA_NO_DIAG
    {
        constexpr ULONGLONG kArmDelayMs = 8000;
        size_t remotePlayers = 0;
        for (const auto& t : targets) if (t.kind == 1) ++remotePlayers;
        static uint64_t armedWorld = 0, seenWorld = 0;
        static ULONGLONG seenAt = 0;
        const ULONGLONG nowArm = GetTickCount64();
        if (seenWorld != gDzWorld) { seenWorld = gDzWorld; seenAt = nowArm; }
        if (armedWorld != gDzWorld && remotePlayers > 0 && nowArm - seenAt >= kArmDelayMs && gDzRoster.empty()) {
            tel::requestDiscover(kQuickSweep);
            armedWorld = gDzWorld;
            logLine("[*] name discovery: %zu remote survivor(s) in range, own name %s - auto sweep queued", remotePlayers,
                    gDzMeName.empty() ? "UNRESOLVED" : ("\"" + gDzMeName + "\"").c_str());
        }
    }
#endif

    DzCamShare cs;
    {
        std::lock_guard<std::mutex> lk(gDzCamMx);
        cs = gDzCamShare;
    }
    const bool camFresh = cs.tUs && qpcUs() - cs.tUs < 250000;
    const Vec3 camPos = cs.c.pos;
    const uint64_t schedUs = qpcUs();
    const float readRange = float(gSettings.dzRange) + 40.f;
    const auto camDist = [&camPos](const Vec3& p) {
        const float dx = p.x - camPos.x, dy = p.y - camPos.y, dz2 = p.z - camPos.z;
        return std::sqrt(dx * dx + dy * dy + dz2 * dz2);
    };
    std::vector<std::array<uint8_t, 16>> flags(targets.size());
    std::vector<std::array<float, 12>> tfs(targets.size());
    std::vector<char> tfOk(targets.size(), 0);
    std::vector<size_t> flagReq(targets.size(), SIZE_MAX);
    std::vector<size_t> posTarget, posReq;
    reqs.clear();
    size_t notDue = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        const uint64_t ptr = targets[i].ptr;
        const auto vit = gDzEntVs.find(ptr);
        const bool haveVs = vit != gDzEntVs.end() && dz::heapPtr(vit->second);
        const auto pit = gDzEntPos.find(ptr);
        bool due = ptr == local || !camFresh || pit == gDzEntPos.end() || !gDzEntDead.count(ptr);
        if (!due) {
            const float d = camDist(pit->second);
            uint64_t budget = 0;
            if (d > readRange) budget = dz::kOutUs;
            else if (d > dz::kNearM) {
                const bool vis = dzMaybeVisible(cs.c, pit->second);
                budget = !vis ? dz::kOffUs : d <= dz::kMidM ? dz::kMidUs : dz::kFarVisUs;
            }
            const auto rit = gDzEntReadUs.find(ptr);
            due = rit == gDzEntReadUs.end() || schedUs - rit->second >= budget;
        }
        if (!due && haveVs) { ++notDue; continue; }
        flagReq[i] = reqs.size();
        reqs.push_back({uintptr_t(ptr + dz::kFlags), flags[i].data(), 16});
        if (!haveVs) continue;
        posTarget.push_back(i);
        posReq.push_back(reqs.size());
        reqs.push_back({uintptr_t(vit->second + dz::kVsTransform), tfs[i].data(), 48});
    }

    const bool wantBones = gSettings.skeleton;
    const float boneRange = float(gSettings.dzRange);
    struct BoneRead {
        size_t ti, req;
        uint64_t arr, arrNow = 0;
        std::array<Vec3, DJ_Count> local;
    };
    std::vector<BoneRead> boneReads;
    boneReads.reserve(posTarget.size());
    std::vector<char> wantSkel(targets.size(), 0);
    if (wantBones) {
        for (const size_t i : posTarget) {
            if (targets[i].kind == 0 || targets[i].kind > 2) continue;
            const auto pit = gDzEntPos.find(targets[i].ptr);
            if (pit == gDzEntPos.end()) continue;
            const Vec3& p = pit->second;
            const float d = camDist(p);
            if (d > boneRange + 10.f) continue;
            if (camFresh && !dzMaybeVisible(cs.c, p)) continue;
            wantSkel[i] = 1;
            const auto cit = gDzEntChain.find(targets[i].ptr);
            if (cit == gDzEntChain.end()) continue;
            if (camFresh && d > dz::kNearM && gDzEntJointOfs.count(targets[i].ptr)) {
                const auto bit = gDzEntBoneUs.find(targets[i].ptr);
                if (bit != gDzEntBoneUs.end() && schedUs - bit->second < dz::kBoneMidUs) continue;
            }
            gDzEntBoneUs[targets[i].ptr] = schedUs;
            BoneRead& br = boneReads.emplace_back();
            br.ti = i;
            br.req = reqs.size();
            br.arr = cit->second.arr;
            const int* tbl = (targets[i].kind == 1) ? dz::kBonePlayer : dz::kBoneInfected;
            for (int b = 0; b < DJ_Count; ++b)
                reqs.push_back({uintptr_t(br.arr + dz::kBoneTrans + uint32_t(tbl[b]) * dz::kBoneStride), &br.local[b], 12});
            reqs.push_back({uintptr_t(cit->second.anim + dz::kMatrixArray), &br.arrNow, 8});
        }
    }
    if (!reqs.empty()) gProc->readMany(reqs);
    const uint64_t posNowUs = qpcUs();
    for (size_t k = 0; k < posTarget.size(); ++k) {
        if (!reqs[posReq[k]].ok) continue;
        const uint64_t ptr = targets[posTarget[k]].ptr;
        tfOk[posTarget[k]] = 1;
        gDzEntReadUs[ptr] = posNowUs;
        const std::array<float, 12>& tf = tfs[posTarget[k]];
        gDzEntFwd[ptr] = {tf[6], tf[8]};
        const Vec3 np{tf[9], tf[10], tf[11]};
        const auto oldP = gDzEntPos.find(ptr);
        const auto oldT = gDzEntPosUs.find(ptr);
        if (oldP != gDzEntPos.end() && oldT != gDzEntPosUs.end()) {
            if (np.x == oldP->second.x && np.y == oldP->second.y && np.z == oldP->second.z) {
                if (posNowUs - oldT->second > 60000) gDzEntVel.erase(ptr);
                continue;
            }
            const double dt = double(posNowUs - oldT->second) / 1e6;
            if (dt > 0.002 && dt < 1.0) {
                Vec3 v{float((np.x - oldP->second.x) / dt), float((np.y - oldP->second.y) / dt),
                       float((np.z - oldP->second.z) / dt)};
                if (v.x * v.x + v.y * v.y + v.z * v.z < 15.0f * 15.0f) {
                    const auto vit = gDzEntVel.find(ptr);
                    if (vit == gDzEntVel.end()) gDzEntVel[ptr] = v;
                    else {
                        vit->second.x = vit->second.x * 0.6f + v.x * 0.4f;
                        vit->second.y = vit->second.y * 0.6f + v.y * 0.4f;
                        vit->second.z = vit->second.z * 0.6f + v.z * 0.4f;
                    }
                } else {
                    gDzEntVel.erase(ptr);
                }
            }
        }
        gDzEntPos[ptr] = np;
        gDzEntPosUs[ptr] = posNowUs;
    }
    out.tUs = posNowUs;

    std::vector<size_t> entOf(targets.size(), SIZE_MAX);
    std::vector<size_t> needNameIdx;
    std::vector<uint64_t> needNamePtr;

    int dead = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        const uint64_t ptr = targets[i].ptr;
        const bool isLocal = (ptr == local);
        bool isDead;
        if (flagReq[i] != SIZE_MAX && reqs[flagReq[i]].ok) {
            isDead = targets[i].kind != 4 && (flags[i][dz::kIsDead - dz::kFlags] & 0x04);
            gDzEntDead[ptr] = isDead;
            uint64_t vs = 0;
            std::memcpy(&vs, &flags[i][dz::kVisualState - dz::kFlags], 8);
            if (dz::heapPtr(vs) && !isDead) gDzEntVs[ptr] = vs;
        } else {
            const auto dit = gDzEntDead.find(ptr);
            if (dit == gDzEntDead.end()) continue;
            isDead = dit->second;
        }
        if (!isLocal && isDead) {
            ++dead;
            continue;
        }
        const auto pit = gDzEntPos.find(ptr);
        if (pit == gDzEntPos.end()) continue;
        if (isLocal) {
            out.me = pit->second;
            out.haveMe = true;
        } else {
            DzSnap::E ee;
            ee.kind = targets[i].kind;
            ee.ptr = ptr;
            ee.feet = pit->second;
            if (const auto fIt = gDzEntFwd.find(ptr); fIt != gDzEntFwd.end()) ee.fwdX = fIt->second.first, ee.fwdZ = fIt->second.second;
            const auto vIt = gDzEntVel.find(ptr);
            if (vIt != gDzEntVel.end()) ee.vel = vIt->second;
            const auto tIt = gDzEntPosUs.find(ptr);
            ee.posUs = tIt != gDzEntPosUs.end() ? tIt->second : posNowUs;
            const auto nIt = gDzEntName.find(ptr);
            if (nIt != gDzEntName.end()) {
                ee.name = nIt->second;
            } else if (ee.kind != 2) {
                needNameIdx.push_back(out.ents.size());
                needNamePtr.push_back(ptr);
            }
            entOf[i] = out.ents.size();
            out.ents.push_back(std::move(ee));
        }
    }
    out.dead = dead;

    for (BoneRead& br : boneReads) {
        const uint64_t ptr = targets[br.ti].ptr;
        if (reqs[br.req + DJ_Count].ok && br.arrNow != br.arr) {
            if (dz::heapPtr(br.arrNow)) gDzEntChain[ptr].arr = br.arrNow;
            else gDzEntChain.erase(ptr);
            continue;
        }
        if (entOf[br.ti] == SIZE_MAX || !tfOk[br.ti]) continue;
        if (!placeJoints(out.ents[entOf[br.ti]], tfs[br.ti].data(), br.local.data(), &reqs[br.req]))
            gDzEntChain.erase(ptr);
    }
    std::vector<BoneJob> boneJobs;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!wantSkel[i] || !tfOk[i] || entOf[i] == SIZE_MAX || gDzEntChain.count(targets[i].ptr)) continue;
        BoneJob& j = boneJobs.emplace_back();
        j.ent = targets[i].ptr;
        j.kind = targets[i].kind;
        j.entIdx = entOf[i];
        std::memcpy(j.tf, tfs[i].data(), sizeof j.tf);
    }
    if (!boneJobs.empty()) {
        readBonesBatch(boneJobs, out);
    }
    if (wantBones) {
        for (DzSnap::E& e : out.ents) {
            if (e.hasBones) continue;
            const auto oit = gDzEntJointOfs.find(e.ptr);
            if (oit == gDzEntJointOfs.end()) continue;
            const float dx = e.feet.x - camPos.x, dy = e.feet.y - camPos.y, dz2 = e.feet.z - camPos.z;
            if (std::sqrt(dx * dx + dy * dy + dz2 * dz2) > boneRange) continue;
            for (int b = 0; b < DJ_Count; ++b)
                e.joints[b] = {e.feet.x + oit->second[b].x, e.feet.y + oit->second[b].y, e.feet.z + oit->second[b].z};
            e.hasBones = true;
        }
    }

#ifndef ICKZA_NO_DIAG
    if (gSettings.dzBoneIds) {
        double best = 1e30;
        size_t bi = SIZE_MAX;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i].kind == 0 || targets[i].kind > 2) continue;
            const auto pit = gDzEntPos.find(targets[i].ptr);
            if (pit == gDzEntPos.end()) continue;
            const Vec3& p = pit->second;
            const double dx = p.x - camPos.x, dy = p.y - camPos.y, dz2 = p.z - camPos.z;
            const double d = dx * dx + dy * dy + dz2 * dz2;
            if (d < best) { best = d; bi = i; }
        }
        if (bi != SIZE_MAX) {
            const auto vit = gDzEntVs.find(targets[bi].ptr);
            if (vit != gDzEntVs.end() && dz::heapPtr(vit->second)) {
                readBonesWorld(targets[bi].ptr, targets[bi].kind, vit->second, out.dbgBones);
                out.dbgKind = targets[bi].kind;
            }
        }
    }
#endif

    constexpr size_t kPName = 48;
    auto cleanName = [](const uint8_t* raw, size_t cap) -> std::string {
        const char* t = reinterpret_cast<const char*>(raw);
        size_t n = 0;
        int letters = 0;
        while (n < cap && t[n]) {
            const unsigned char c = static_cast<unsigned char>(t[n]);
            if (c < 0x20 || c >= 0x7F) break;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++letters;
            ++n;
        }
        return (n >= 2 && letters >= 2) ? std::string(t, n) : std::string();
    };
    if (!needNamePtr.empty()) {
        std::vector<uint64_t> cfg(needNamePtr.size(), 0), str(needNamePtr.size(), 0);
        reqs.clear();
        for (size_t k = 0; k < needNamePtr.size(); ++k)
            reqs.push_back({uintptr_t(needNamePtr[k] + dz::kItemCfg), &cfg[k], 8});
        gProc->readMany(reqs);
        reqs.clear();
        for (size_t k = 0; k < needNamePtr.size(); ++k)
            reqs.push_back({uintptr_t(dz::heapPtr(cfg[k]) ? cfg[k] + dz::kCfgName : 0), &str[k], 8});
        gProc->readMany(reqs);
        std::vector<uint8_t> nb(needNamePtr.size() * kPName, 0);
        reqs.clear();
        for (size_t k = 0; k < needNamePtr.size(); ++k)
            reqs.push_back({uintptr_t(dz::heapPtr(str[k]) ? str[k] + dz::kNameText : 0), &nb[k * kPName], kPName});
        gProc->readMany(reqs);
        for (size_t k = 0; k < needNamePtr.size(); ++k) {
            std::string nm = reqs[k].ok ? cleanName(&nb[k * kPName], kPName) : std::string();
            if (nm.rfind("SurvivorM_", 0) == 0 || nm.rfind("SurvivorF_", 0) == 0)
                nm = nm.substr(10);
            nm = dzPretty(out.ents[needNameIdx[k]].kind, nm);
            gDzEntName[needNamePtr[k]] = nm;
            out.ents[needNameIdx[k]].name = nm;
        }
    }

    if (now - gDzRosterTick >= dz::kRosterMs) {
        gDzRosterTick = now;
        std::unordered_map<uint32_t, std::string> fresh;
        bool rosterRead = false;
        uint64_t nc = 0, arr = 0;
        if (gProc->read(uintptr_t(base + dz::kNetClient), nc) && dz::heapPtr(nc) && gProc->read(uintptr_t(nc + dz::kNcRoster), arr) &&
            dz::heapPtr(arr)) {
            int32_t cnt = 0;
            gProc->read(uintptr_t(nc + dz::kNcRoster + 8), cnt);
            if (cnt <= 0 || cnt > dz::kMaxRoster) cnt = dz::kMaxRoster;
            std::vector<uint64_t> ids(size_t(cnt), 0);
            if (gProc->read(uintptr_t(arr), ids.data(), ids.size() * 8)) {
                std::vector<uint64_t> live;
                for (uint64_t id : ids)
                    if (dz::heapPtr(id)) live.push_back(id);
                std::vector<uint32_t> nid(live.size(), 0);
                std::vector<uint64_t> str(live.size(), 0);
                reqs.clear();
                for (size_t k = 0; k < live.size(); ++k) {
                    reqs.push_back({uintptr_t(live[k] + dz::kIdNetId), &nid[k], 4});
                    reqs.push_back({uintptr_t(live[k] + dz::kIdName), &str[k], 8});
                }
                gProc->readMany(reqs);
                std::vector<uint8_t> nb(live.size() * kPName, 0);
                reqs.clear();
                for (size_t k = 0; k < live.size(); ++k)
                    reqs.push_back({uintptr_t(dz::heapPtr(str[k]) ? str[k] + dz::kNameText : 0), &nb[k * kPName], kPName});
                gProc->readMany(reqs);
#ifndef ICKZA_NO_DIAG
                size_t anon = SIZE_MAX;
#endif
                for (size_t k = 0; k < live.size(); ++k) {
                    const std::string nm = reqs[k].ok ? cleanName(&nb[k * kPName], kPName) : std::string();
                    if (!nm.empty() && nid[k]) fresh[nid[k]] = nm;
#ifndef ICKZA_NO_DIAG
                    if (anon == SIZE_MAX && nm.rfind("Survivor", 0) == 0) anon = k;
#endif
                }
                rosterRead = true;
#ifndef ICKZA_NO_DIAG
                static uint64_t probedWorld = 0;
                if (probedWorld != world && !fresh.empty()) {
                    probedWorld = world;
                    std::string all;
                    int anonCount = 0;
                    for (const auto& kv : fresh) {
                        anonCount += kv.second.rfind("Survivor", 0) == 0;
                        if (all.size() < 1800) all += (all.empty() ? "" : ", ") + kv.second;
                    }
                    logLine("[log] roster names (%zu, %d \"Survivor\"): %s", fresh.size(), anonCount, all.c_str());
                    if (anon != SIZE_MAX) {
                        constexpr uint32_t kIdBytes = 0x180;
                        std::vector<uint64_t> slot(kIdBytes / 8, 0);
                        gProc->read(uintptr_t(live[anon]), slot.data(), kIdBytes);
                        std::vector<uint8_t> tb(slot.size() * kPName, 0);
                        reqs.clear();
                        for (size_t s = 0; s < slot.size(); ++s)
                            reqs.push_back({uintptr_t(dz::heapPtr(slot[s]) ? slot[s] + dz::kNameText : 0), &tb[s * kPName], kPName});
                        gProc->readMany(reqs);
                        std::string fields;
                        for (size_t s = 0; s < slot.size(); ++s) {
                            const std::string t = reqs[s].ok ? cleanName(&tb[s * kPName], kPName) : std::string();
                            if (!t.empty()) fields += "  +0x" + [](size_t v) { char b[8]; std::snprintf(b, sizeof b, "%zX", v); return std::string(b); }(s * 8) + "=\"" + t + "\"";
                        }
                        logLine("[log] roster probe: identity %llX (net id %u) text fields:%s", (unsigned long long)live[anon], nid[anon],
                                fields.empty() ? " none" : fields.c_str());
                    }
                }
#endif
            }
        }
        if (rosterRead) {
            gDzRoster = std::move(fresh);
            auto names = std::make_shared<std::vector<std::string>>();
            for (const auto& kv : gDzRoster) names->push_back(kv.second);
            std::sort(names->begin(), names->end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
            if (!gDzOnline || *gDzOnline != *names) gDzOnline = std::move(names);
        }
    }
    if (!gDzRoster.empty()) {
        std::vector<size_t> needId;
        for (size_t k = 0; k < out.ents.size(); ++k)
            if (out.ents[k].kind == 1 && !gDzEntNetId.count(out.ents[k].ptr)) needId.push_back(k);
        if (!needId.empty()) {
            std::vector<uint32_t> nid(needId.size(), 0);
            reqs.clear();
            for (size_t k = 0; k < needId.size(); ++k) reqs.push_back({uintptr_t(out.ents[needId[k]].ptr + dz::kEntNetId), &nid[k], 4});
            gProc->readMany(reqs);
            for (size_t k = 0; k < needId.size(); ++k)
                if (reqs[k].ok) gDzEntNetId[out.ents[needId[k]].ptr] = nid[k];
        }
        for (DzSnap::E& e : out.ents) {
            if (e.kind != 1) continue;
            const auto idIt = gDzEntNetId.find(e.ptr);
            if (idIt == gDzEntNetId.end()) continue;
            const auto nmIt = gDzRoster.find(idIt->second);
            if (nmIt != gDzRoster.end()) e.name = nmIt->second;
        }
#ifndef ICKZA_NO_DIAG
        static ULONGLONG lastRosterLog = 0;
        if (now - lastRosterLog >= 10000) {
            lastRosterLog = now;
            int survivors = 0, named = 0;
            std::string sample;
            for (const DzSnap::E& e : out.ents) {
                if (e.kind != 1) continue;
                ++survivors;
                const auto idIt = gDzEntNetId.find(e.ptr);
                if (idIt == gDzEntNetId.end() || !gDzRoster.count(idIt->second)) continue;
                ++named;
                if (sample.size() < 160) sample += (sample.empty() ? "" : ", ") + e.name;
            }
            logLine("[log] roster: %zu players on the server; %d of %d survivors named: %s", gDzRoster.size(), named, survivors,
                    sample.c_str());
        }
#endif
        if (local && gDzMeName.empty()) {
            uint32_t myId = 0;
            if (gProc->read(uintptr_t(local + dz::kEntNetId), myId) && myId) {
                const auto nmIt = gDzRoster.find(myId);
                if (nmIt != gDzRoster.end()) gDzMeName = nmIt->second;
            }
        }
    }

    if (now - gDzServerTick >= 5000) {
        gDzServerTick = now;
        constexpr size_t kSName = 96;
        uint64_t nc = 0, str = 0;
        uint8_t tb[kSName] = {};
        std::string best;
        if (gProc->read(uintptr_t(base + dz::kNetClient), nc) && dz::heapPtr(nc) &&
            gProc->read(uintptr_t(nc + dz::kNcServerName), str) && dz::heapPtr(str) &&
            gProc->read(uintptr_t(str + dz::kNameText), tb, kSName - 1))
            best = cleanName(tb, kSName);
        std::lock_guard<std::mutex> lk(gDzServerMx);
        gDzServer = best;
    }

    if (gSettings.dzHeld) {
        if (now - gDzHeldTick >= dz::kHeldMs) {
            gDzHeldTick = now;
            std::vector<uint64_t> whoPtr;
            for (const DzSnap::E& e : out.ents)
                if (e.kind == 1) whoPtr.push_back(e.ptr);
            if (local) whoPtr.push_back(local);
            std::vector<uint64_t> item(whoPtr.size(), 0);
            reqs.clear();
            for (size_t k = 0; k < whoPtr.size(); ++k) reqs.push_back({uintptr_t(whoPtr[k] + dz::kHeldA), &item[k], 8});
            if (!reqs.empty()) gProc->readMany(reqs);
            reqs.clear();
            for (size_t k = 0; k < whoPtr.size(); ++k)
                reqs.push_back({uintptr_t(dz::heapPtr(item[k]) ? item[k] + dz::kHeldB : 0), &item[k], 8});
            if (!reqs.empty()) gProc->readMany(reqs);
            std::vector<uint64_t> unnamed;
            for (uint64_t it : item)
                if (dz::heapPtr(it) && !gDzItemName.count(it)) unnamed.push_back(it);
            if (!unnamed.empty()) {
                const auto names = dzTypeNames(unnamed);
                for (size_t k = 0; k < unnamed.size(); ++k) gDzItemName[unnamed[k]] = names[k];
            }
            if (gDzItemName.size() > 4096) gDzItemName.clear();
            gDzEntHeld.clear();
            for (size_t k = 0; k < whoPtr.size(); ++k) {
                std::string nm;
                if (dz::heapPtr(item[k])) {
                    const auto it = gDzItemName.find(item[k]);
                    nm = (it != gDzItemName.end() && !it->second.empty()) ? it->second : "?";
                }
                gDzEntHeld[whoPtr[k]] = nm;
            }
#ifndef ICKZA_NO_DIAG
            static ULONGLONG lastHeldLog = 0;
            if (now - lastHeldLog >= 5000) {
                lastHeldLog = now;
                int holding = 0, noName = 0;
                std::string sample;
                for (size_t k = 0; k < whoPtr.size(); ++k) {
                    const std::string& nm = gDzEntHeld[whoPtr[k]];
                    if (nm.empty()) continue;
                    ++holding;
                    if (nm == "?") ++noName;
                    if (whoPtr[k] != local && sample.size() < 160) sample += (sample.empty() ? "" : ", ") + nm;
                }
                const std::string mine = local ? gDzEntHeld[local] : std::string();
                logLine("[log] held: you \"%s\"; %d of %zu holding something (%d unnamed): %s", mine.c_str(), holding, whoPtr.size(),
                        noName, sample.c_str());
            }
#endif
        }
        for (DzSnap::E& e : out.ents) {
            const auto h = gDzEntHeld.find(e.ptr);
            if (h != gDzEntHeld.end()) e.held = h->second;
        }
        if (local) {
            const auto h = gDzEntHeld.find(local);
            if (h != gDzEntHeld.end()) out.meHeld = h->second;
        }
    }

    if (now - gDzMeNameTick >= 1000) {
        gDzMeNameTick = now;
        uint64_t a = 0, b = 0, c = 0, slot = 0;
        if (gProc->read(uintptr_t(world + dz::kMeHub), a) && dz::heapPtr(a) &&
            gProc->read(uintptr_t(a + dz::kMeA), b) && dz::heapPtr(b) &&
            gProc->read(uintptr_t(b + dz::kMeB), c) && dz::heapPtr(c) &&
            gProc->read(uintptr_t(c + dz::kMeSlot), slot) && dz::heapPtr(slot)) {
            uint8_t mb[kPName] = {};
            if (gProc->read(uintptr_t(slot + dz::kNameText), mb, kPName)) {
                std::string nm = cleanName(mb, kPName);
                if (!nm.empty()) gDzMeName = nm;
            }
        }
#ifndef ICKZA_NO_DIAG
        const int hop = !dz::heapPtr(a) ? 0 : !dz::heapPtr(b) ? 1 : !dz::heapPtr(c) ? 2 : !dz::heapPtr(slot) ? 3 : gDzMeName.empty() ? 4 : 5;
        static int lastHop = -1;
        static std::string lastName;
        if (hop != lastHop || (hop == 5 && gDzMeName != lastName)) {
            lastHop = hop;
            lastName = gDzMeName;
            if (hop == 5)
                logLine("[*] own name: \"%s\"", gDzMeName.c_str());
            else
                logLine("[!] own name chain stops at hop %d (World+0x%X=%llX ->+0x%X=%llX ->+0x%X=%llX ->+0x%X=%llX)", hop,
                        dz::kMeHub, (unsigned long long)a, dz::kMeA, (unsigned long long)b, dz::kMeB, (unsigned long long)c,
                        dz::kMeSlot, (unsigned long long)slot);
        }
#endif
    }
    out.meName = gDzMeName;
    out.online = gDzOnline;

    gDzWorldPub.store(world, std::memory_order_relaxed);
    if (gSettings.dzLoot) {
        if (!gDzLootThread.joinable()) dzLootPass(world);
        std::lock_guard<std::mutex> lk(gDzLootMx);
        out.loot = gDzLootList;
        out.lootSlots = gDzLootSlotsPub;
    }
    stage = "ok";
}

constexpr uint32_t kLootStride = 0x18, kLootEntPtr = 0x8;

uint32_t dzLootLayout(const uint8_t* raw, size_t bytes) {
    size_t n = bytes / kLootStride, sane = 0, live = 0;
    for (size_t i = 0; i < n; ++i) {
        uint16_t f;
        uint64_t p;
        std::memcpy(&f, raw + i * kLootStride, 2);
        std::memcpy(&p, raw + i * kLootStride + kLootEntPtr, 8);
        if ((f == 0 || f == 1) && (p == 0 || dz::heapPtr(p))) ++sane;
        if (f == 1 && dz::heapPtr(p)) ++live;
    }
    return n >= 4 && live > 0 && sane * 10 >= n * 9 ? kLootStride : 8;
}

void dzLootPass(uint64_t world) {
    const ULONGLONG now = GetTickCount64();
    std::vector<DzLootItem> list;
    size_t lootSlots = 0;
    {
        const bool lootSweep = now - gDzLootSweepTick >= 10000;
        if (lootSweep) gDzLootSweepTick = now;
        static uint32_t stride = 0;
        if (lootSweep) stride = 0;
        uint64_t hdr[3] = {};
        std::vector<Process::ReadReq> lreq = {{uintptr_t(world + dz::kLoot), hdr, 24}};
        gProc->readMany(lreq);
        const uint64_t lbuf = hdr[0], alloc = hdr[1] & 0xFFFFFFFF, lvalid = hdr[2] & 0xFFFFFFFF;
#ifndef ICKZA_NO_DIAG
        {
            static uint64_t lastHdr[3] = {};
            if (lreq[0].ok && std::memcmp(lastHdr, hdr, sizeof hdr) != 0) {
                std::memcpy(lastHdr, hdr, sizeof hdr);
                logLine("[loot] table header: buf %llX  +8 %08X %08X  +0x10 %08X %08X", (unsigned long long)hdr[0], unsigned(hdr[1]),
                        unsigned(hdr[1] >> 32), unsigned(hdr[2]), unsigned(hdr[2] >> 32));
            }
        }
#endif
        bool slotsOk = false;
        std::vector<uint64_t> lslots;
        if (lreq[0].ok && dz::heapPtr(lbuf) && alloc && alloc <= dz::kMaxLoot) {
            if (stride == 0) {
                std::vector<uint8_t> probe(alloc * 8, 0);
                lreq = {{uintptr_t(lbuf), probe.data(), probe.size()}};
                gProc->readMany(lreq);
                if (lreq[0].ok) {
                    stride = dzLootLayout(probe.data(), probe.size());
#ifndef ICKZA_NO_DIAG
                    static uint32_t logged = 0;
                    if (stride != logged)
                        logLine("[loot] item table: %s entries, alloc %llu, valid %llu",
                                stride == kLootStride ? "flagged 0x18" : "flat 8-byte", (unsigned long long)alloc,
                                (unsigned long long)lvalid);
                    logged = stride;
#endif
                }
            }
            if (stride == kLootStride) {
                std::vector<uint8_t> raw(alloc * kLootStride, 0);
                lreq = {{uintptr_t(lbuf), raw.data(), raw.size()}};
                gProc->readMany(lreq);
                slotsOk = lreq[0].ok;
                if (slotsOk) {
                    lslots.reserve(lvalid && lvalid <= alloc ? lvalid : alloc);
                    for (size_t i = 0; i < alloc; ++i) {
                        uint16_t f;
                        uint64_t p;
                        std::memcpy(&f, &raw[i * kLootStride], 2);
                        std::memcpy(&p, &raw[i * kLootStride + kLootEntPtr], 8);
                        if (f == 1 && dz::heapPtr(p)) lslots.push_back(p);
                    }
                }
            } else if (stride == 8) {
                const uint64_t count = hdr[1] >> 32;
                const uint64_t n = count && count <= alloc ? count : alloc;
                lslots.assign(n, 0);
                lreq = {{uintptr_t(lbuf), lslots.data(), n * 8}};
                gProc->readMany(lreq);
                slotsOk = lreq[0].ok;
            }
        }
#ifndef ICKZA_NO_DIAG
        if (slotsOk) {
            static std::unordered_set<uint64_t> prevSlots;
            std::unordered_set<uint64_t> cur;
            for (uint64_t s : lslots) if (dz::heapPtr(s)) cur.insert(s);
            size_t added = 0, removed = 0;
            for (uint64_t s : cur) added += !prevSlots.count(s);
            for (uint64_t s : prevSlots) removed += !cur.count(s);
            if ((added || removed) && !prevSlots.empty())
                logLine("[loot] list: +%zu -%zu (now %zu of %zu slots)", added, removed, cur.size(), lslots.size());
            prevSlots.swap(cur);
        }
#endif
        if (slotsOk && !lslots.empty()) {
            const uint64_t ln = lslots.size();
            lootSlots = ln;
            if (lootSweep) {
                const std::unordered_set<uint64_t> live(lslots.begin(), lslots.end());
                const auto prune = [&live](auto& m) {
                    for (auto it = m.begin(); it != m.end();) it = live.count(it->first) ? std::next(it) : m.erase(it);
                };
                prune(gDzLootVs);
                prune(gDzLootPos);
                prune(gDzLootName);
                prune(gDzLootCat);
            }

            std::vector<uint64_t> needVs;
            for (uint64_t s : lslots)
                if (dz::heapPtr(s) && gDzLootVs.find(s) == gDzLootVs.end()) needVs.push_back(s);
            const size_t nNew = needVs.size();
            static size_t lootCheckAt = 0;
            static ULONGLONG lootCheckTick = 0;
            if (now - lootCheckTick >= 250) {
                lootCheckTick = now;
                for (size_t n = 0; n < 64 && n < ln; ++n) {
                    const uint64_t s = lslots[(lootCheckAt + n) % ln];
                    if (gDzLootVs.count(s)) needVs.push_back(s);
                }
                lootCheckAt = (lootCheckAt + 64) % ln;
            }
            if (!needVs.empty()) {
                std::vector<uint64_t> vs(needVs.size(), 0);
                lreq.clear();
                for (size_t i = 0; i < needVs.size(); ++i) lreq.push_back({uintptr_t(needVs[i] + dz::kVisualState), &vs[i], 8});
                gProc->readMany(lreq);
                for (size_t i = 0; i < needVs.size(); ++i) {
                    if (!lreq[i].ok || !dz::heapPtr(vs[i])) continue;
                    if (i >= nNew && gDzLootVs[needVs[i]] != vs[i]) {
                        gDzLootPos.erase(needVs[i]);
                        gDzLootName.erase(needVs[i]);
                        gDzLootCat.erase(needVs[i]);
                    }
                    gDzLootVs[needVs[i]] = vs[i];
                }
            }
            std::vector<uint64_t> needPos;
            for (uint64_t s : lslots) {
                const auto v = gDzLootVs.find(s);
                if (v != gDzLootVs.end() && gDzLootPos.find(s) == gDzLootPos.end()) needPos.push_back(s);
            }
            if (!needPos.empty()) {
                std::vector<Vec3> pv(needPos.size());
                lreq.clear();
                for (size_t i = 0; i < needPos.size(); ++i) lreq.push_back({uintptr_t(gDzLootVs[needPos[i]] + dz::kVsPos), &pv[i], 12});
                gProc->readMany(lreq);
                for (size_t i = 0; i < needPos.size(); ++i)
                    if (lreq[i].ok && std::isfinite(pv[i].x)) gDzLootPos[needPos[i]] = pv[i];
            }
            std::vector<uint64_t> needName;
            for (uint64_t s : lslots)
                if (dz::heapPtr(s) && gDzLootName.find(s) == gDzLootName.end()) needName.push_back(s);
            if (needName.size() > kLootNamesPerPass) needName.resize(kLootNamesPerPass);
            if (!needName.empty()) {
                std::vector<uint64_t> cfg(needName.size(), 0), str(needName.size(), 0), ivt(needName.size(), 0);
                lreq.clear();
                for (size_t i = 0; i < needName.size(); ++i)
                    lreq.push_back({uintptr_t(needName[i] + dz::kItemCfg), &cfg[i], 8});
                for (size_t i = 0; i < needName.size(); ++i)
                    lreq.push_back({uintptr_t(needName[i]), &ivt[i], 8});
                gProc->readMany(lreq);
                lreq.clear();
                for (size_t i = 0; i < needName.size(); ++i)
                    lreq.push_back({uintptr_t(dz::heapPtr(cfg[i]) ? cfg[i] + dz::kCfgName : 0), &str[i], 8});
                gProc->readMany(lreq);
                constexpr size_t kNameBytes = 48;
                std::vector<uint8_t> nb(needName.size() * kNameBytes, 0);
                lreq.clear();
                for (size_t i = 0; i < needName.size(); ++i)
                    lreq.push_back({uintptr_t(dz::heapPtr(str[i]) ? str[i] + dz::kNameText : 0), &nb[i * kNameBytes], kNameBytes});
                gProc->readMany(lreq);
                for (size_t i = 0; i < needName.size(); ++i) {
                    const char* t = reinterpret_cast<const char*>(&nb[i * kNameBytes]);
                    size_t n = 0;
                    while (n < kNameBytes && t[n]) {
                        const unsigned char c = static_cast<unsigned char>(t[n]);
                        if (c < 0x20 || c >= 0x7F) break;
                        ++n;
                    }
                    bool ident = lreq[i].ok && n >= 3;
                    for (size_t k = 0; ident && k < n; ++k) {
                        const unsigned char c = static_cast<unsigned char>(t[k]);
                        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                        const bool digit = c >= '0' && c <= '9';
                        if (!alpha && !digit) ident = false;
                    }
                    if (ident) {
                        const unsigned char c0 = static_cast<unsigned char>(t[0]);
                        ident = (c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z');
                    }
                    gDzLootName[needName[i]] = ident ? std::string(t, n) : std::string();
                    const auto vc = gDzLootVts.find(ivt[i]);
                    gDzLootCat[needName[i]] = dzLootCategory(vc == gDzLootVts.end() ? LC_Other : vc->second, gDzLootName[needName[i]]);
                }
            }
            for (uint64_t s : lslots) {
                const auto p = gDzLootPos.find(s);
                if (p == gDzLootPos.end()) continue;
                DzLootItem li;
                li.pos = p->second;
                const auto nm = gDzLootName.find(s);
                if (nm != gDzLootName.end()) li.name = nm->second;
                const auto ct = gDzLootCat.find(s);
                if (ct != gDzLootCat.end()) li.cat = ct->second;
                list.push_back(std::move(li));
            }
        }
    }
    auto pub = std::make_shared<const std::vector<DzLootItem>>(std::move(list));
    std::lock_guard<std::mutex> lk(gDzLootMx);
    gDzLootList = std::move(pub);
    gDzLootSlotsPub = lootSlots;
}

bool readDzCameraRaw(uint64_t cam, dz::Cam& c) {
    if (!cam) return false;
    uint8_t camBytes[dz::kCamBytes];
    std::vector<Process::ReadReq> reqs = {{uintptr_t(cam), camBytes, sizeof camBytes}};
    gProc->readMany(reqs);
    return reqs[0].ok && dz::parseCam(camBytes, c);
}

bool readDzCamera(uint64_t cam, dz::Cam& c) {
    if (!readDzCameraRaw(cam, c)) return false;
    const uint64_t now = qpcUs();
    std::lock_guard<std::mutex> lk(gDzCamMx);
    gDzCamShare = DzCamShare{c, now, now};
    return true;
}

dz::Cam advanceCam(const dz::Cam& c, const Vec3& omega, const Vec3& vel, float L) {
    dz::Cam view = c;
    float ang = dz::vlen(omega) * L;
    if (ang > 0.25f) ang = 0.25f;
    if (ang > 1e-5f) {
        const Vec3 ax = dz::vscale(omega, 1.f / dz::vlen(omega));
        view.right = dz::rodrigues(c.right, ax, ang);
        view.up = dz::rodrigues(c.up, ax, ang);
        view.fwd = dz::rodrigues(c.fwd, ax, ang);
    }
    view.pos = dz::vadd(c.pos, dz::vscale(vel, L));
    return view;
}

Vec3 camOmega(const dz::Cam& a, const dz::Cam& b, float dt) {
    const Vec3 wv = dz::vadd(dz::vadd(dz::vcross(a.right, b.right), dz::vcross(a.up, b.up)), dz::vcross(a.fwd, b.fwd));
    return dz::vscale(wv, 0.5f / dt);
}

constexpr int kDzCamMinHz = 150, kDzCamMaxHz = 500;
std::thread gDzCamThread;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void dzCamLoop() {
    markUrgentReadThread();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    uint64_t period = 1000000 / kDzCamMaxHz;
    double gameIntervalUs = 0;
    dz::Cam prev{};
    uint64_t prevChangeUs = 0;
    Vec3 omega{}, vel{};
    uint64_t next = qpcUs();
    while (gDzReadRun.load(std::memory_order_relaxed)) {
        uint64_t cam = 0;
        Vec3 me{};
        {
            std::lock_guard<std::mutex> lk(gDzSnapMx);
            cam = gDzSnap.cam;
            me = gDzSnap.me;
        }
        const uint64_t t0 = qpcUs();
        dz::Cam c{};
        bool ok = cam && readDzCameraRaw(cam, c);
        const uint64_t t1 = qpcUs();
        static uint64_t farSinceUs = 0;
        if (ok && (me.x != 0 || me.z != 0)) {
            const float dx = c.pos.x - me.x, dy = c.pos.y - me.y, dz2 = c.pos.z - me.z;
            if (dx * dx + dy * dy + dz2 * dz2 > 30.f * 30.f) {
                if (!farSinceUs) farSinceUs = t1;
                if (t1 - farSinceUs < 1000000) ok = false;
            } else {
                farSinceUs = 0;
            }
        }
        tel::time(tel::T_CAM, t1 - t0);
#ifndef ICKZA_NO_DIAG
        gDzCamMs = gDzCamMs.load() * 0.98 + double(t1 - t0) / 1000.0 * 0.02;
#endif
#ifndef ICKZA_NO_DIAG
        {
            static uint64_t winStart = 0, lastPtr = 0;
            static int changes = 0, jumps = 0, ptrSwaps = 0;
            static float maxDeg = 0, maxM = 0, pxMin = 1e9f, pxMax = -1e9f;
            if (cam != lastPtr) { ptrSwaps += lastPtr != 0; lastPtr = cam; }
            if (ok && prevChangeUs && std::memcmp(&c, &prev, sizeof c) != 0) {
                ++changes;
                const float deg = std::acos(std::clamp(dz::vdot(c.fwd, prev.fwd), -1.f, 1.f)) * 57.2958f;
                const float m = dz::vlen(dz::vsub(c.pos, prev.pos));
                maxDeg = std::max(maxDeg, deg);
                maxM = std::max(maxM, m);
                jumps += deg > 8.f || m > 0.5f;
            }
            if (ok) { pxMin = std::min(pxMin, c.px); pxMax = std::max(pxMax, c.px); }
            if (t1 - winStart >= 1000000) {
                if (winStart && (jumps || ptrSwaps || pxMax - pxMin > 0.01f))
                    logLine("[cam] 1s: %d changes, %d jumps (max %.1f deg, %.2f m), fov tan %.3f..%.3f, Camera* swaps %d (now %llX)", changes,
                            jumps, maxDeg, maxM, pxMin, pxMax, ptrSwaps, (unsigned long long)cam);
                winStart = t1;
                changes = jumps = ptrSwaps = 0;
                maxDeg = maxM = 0;
                pxMin = 1e9f;
                pxMax = -1e9f;
            }
        }
#endif
        if (ok) {
            if (!prevChangeUs || std::memcmp(&c, &prev, sizeof c) != 0) {
                const double dt = prevChangeUs ? double(t1 - prevChangeUs) / 1e6 : 0.0;
                if (dt > 0.0005 && dt < 0.1) {
                    omega = dz::vlerp(omega, camOmega(prev, c, float(dt)), 0.5f);
                    vel = dz::vlerp(vel, dz::vscale(dz::vsub(c.pos, prev.pos), 1.f / float(dt)), 0.5f);
                    if (dt < 0.05) {
                        gameIntervalUs = gameIntervalUs > 0 ? gameIntervalUs * 0.95 + dt * 1e6 * 0.05 : dt * 1e6;
                        period = std::clamp(uint64_t(gameIntervalUs / 2), uint64_t(1000000 / kDzCamMaxHz), uint64_t(1000000 / kDzCamMinHz));
                    }
                } else {
                    omega = vel = Vec3{};
                }
                prev = c;
                prevChangeUs = t1;
            } else if (t1 - prevChangeUs > 50000) {
                omega = vel = Vec3{};
            }
            std::lock_guard<std::mutex> lk(gDzCamMx);
            gDzCamShare = DzCamShare{prev, t1, prevChangeUs, omega, vel};
        }
        next += period;
        const uint64_t now = qpcUs();
        if (next < now) next = now;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG(next - now) * 10;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
            else Sleep(1);
        } else {
            Sleep(1);
        }
    }
    if (timer) CloseHandle(timer);
}

dz::Cam dzViewCam(const DzCamShare& s, uint64_t nowUs) {
    if (gSettings.predictMs <= 0) return s.c;
    const double age = nowUs > s.tChangeUs ? std::min(double(nowUs - s.tChangeUs) / 1e6, 0.05) : 0.0;
    return advanceCam(s.c, s.omega, s.vel, float(gSettings.predictMs / 1000.0 + age));
}

dz::Cam predictCam(const dz::Cam& c) {
    dz::Cam view = c;
    if (gSettings.predictMs <= 0) return view;
    static bool havePrev = false;
    static dz::Cam prev;
    static LARGE_INTEGER prevQpc{}, qpf{};
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER nowQpc;
    QueryPerformanceCounter(&nowQpc);
    if (havePrev) {
        const double dt = double(nowQpc.QuadPart - prevQpc.QuadPart) / double(qpf.QuadPart);
        if (dt > 0.0005 && dt < 0.1) {
            gDzOmega = dz::vlerp(gDzOmega, camOmega(prev, c, float(dt)), 0.5f);
            gDzVel = dz::vlerp(gDzVel, dz::vscale(dz::vsub(c.pos, prev.pos), 1.f / float(dt)), 0.5f);
            view = advanceCam(c, gDzOmega, gDzVel, gSettings.predictMs / 1000.f);
        }
    }
    prev = c;
    prevQpc = nowQpc;
    havePrev = true;
    return view;
}

void projectDz(const DzSnap& snap, const dz::Cam& view, const Vec3& me) {
    gBoxes.clear();
    const float range = float(gSettings.dzRange);
    const float w = float(gView.right - gView.left), h = float(gView.bottom - gView.top);
    const uint64_t nowUs = qpcUs();
    for (const DzSnap::E& ent : snap.ents) {
        if (ent.kind == 2 && !gSettings.dzInfected) continue;
        Vec3 feet = ent.feet;
        Vec3 lead{0, 0, 0};
        if (ent.posUs) {
            const double age = std::clamp(double(nowUs > ent.posUs ? nowUs - ent.posUs : 0) / 1e6, 0.0, 0.30);
            lead = {float(ent.vel.x * age), float(ent.vel.y * age), float(ent.vel.z * age)};
            feet.x += lead.x;
            feet.y += lead.y;
            feet.z += lead.z;
        }
        if (!std::isfinite(feet.x) || std::fabs(feet.x) > 50000 || std::fabs(feet.z) > 50000) continue;
        const float dx = feet.x - me.x, dy = feet.y - me.y, dzz = feet.z - me.z;
        const float dist = std::sqrt(dx * dx + dy * dy + dzz * dzz);
        if (dist > range) continue;
        const float height = ent.kind == 3 ? 1.0f : ent.kind == 4 ? 2.0f : dz::kHeight;
        const float aspect = ent.kind == 3 ? 1.3f : ent.kind == 4 ? 2.2f : 0.45f;
        const Vec3 top{feet.x, feet.y + height, feet.z};
        float tx, ty, fx, fy;
        if (!dz::toScreen(view, top, w, h, tx, ty) || !dz::toScreen(view, feet, w, h, fx, fy)) continue;
        const float bh = fy - ty;
        if (bh < 2) continue;
        const float bw = bh * aspect;
        Ent e{};
        e.pos = top;
        e.feet = feet;
        static const char* const kDefault[] = {"", "survivor", "zombie", "animal", "vehicle"};
        e.name = !ent.name.empty() ? ent.name : kDefault[std::clamp(ent.kind, 0, 4)];
        Box b{gView.left + tx - bw / 2, gView.top + ty, bw, bh, e, dist, ent.held, true, Skeleton{}};
        b.kind = ent.kind;
        if (ent.hasBones) {
            for (int j = 0; j < DJ_Count; ++j) {
                float jx, jy;
                const Vec3 jp{ent.joints[j].x + lead.x, ent.joints[j].y + lead.y, ent.joints[j].z + lead.z};
                if (std::isfinite(ent.joints[j].x) && dz::toScreen(view, jp, w, h, jx, jy)) {
                    b.skel.pt[j] = {LONG(gView.left + jx), LONG(gView.top + jy)};
                    b.skel.ok[j] = true;
                } else {
                    b.skel.ok[j] = false;
                }
            }
            if (b.skel.ok[DJ_Head] && b.skel.ok[DJ_Neck])
                b.skel.headRadius = std::max(2, int(std::abs(b.skel.pt[DJ_Head].y - b.skel.pt[DJ_Neck].y)));
            {
                float jx0 = 1e9f, jy0 = 1e9f, jx1 = -1e9f, jy1 = -1e9f;
                int jn = 0;
                for (int j = 0; j < DJ_Count; ++j) {
                    if (!b.skel.ok[j]) continue;
                    jx0 = std::min(jx0, (float)b.skel.pt[j].x);
                    jx1 = std::max(jx1, (float)b.skel.pt[j].x);
                    jy0 = std::min(jy0, (float)b.skel.pt[j].y);
                    jy1 = std::max(jy1, (float)b.skel.pt[j].y);
                    ++jn;
                }
                if (jn >= 6) {
                    const float jh = jy1 - jy0;
                    const float padX = std::max(4.0f, jh * 0.12f), padY = std::max(3.0f, jh * 0.06f);
                    b.x = jx0 - padX;
                    b.y = jy0 - padY;
                    b.w = (jx1 - jx0) + 2 * padX;
                    b.h = jh + 2 * padY;
                }
            }
        }
        gBoxes.push_back(b);
    }

    gDzLootPts.clear();
    if (gSettings.dzLoot && snap.loot) {
        float lootRange = range;
        if (gSettings.dzLootRange > 0 && float(gSettings.dzLootRange) < lootRange) lootRange = float(gSettings.dzLootRange);
        const bool whitelist = loot::filter().mode == loot::FilterWhite;
        for (const DzLootItem& it : *snap.loot) {
            const Vec3& p = it.pos;
            if (!std::isfinite(p.x) || std::fabs(p.x) > 50000 || std::fabs(p.z) > 50000) continue;
            const float dx = p.x - me.x, dy = p.y - me.y, dzz = p.z - me.z;
            const float dist = std::sqrt(dx * dx + dy * dy + dzz * dzz);
            if (dist > lootRange && it.cat != LC_Rare) continue;
            if (dist > range) continue;
            if (gSettings.dzLootValuables && !whitelist && !loot::valuable(it.cat)) continue;
            if (!loot::visible(gSettings.dzLootCats, it.cat, it.name)) continue;
            float sx, sy;
            if (!dz::toScreen(view, p, w, h, sx, sy)) continue;
            gDzLootPts.push_back({{LONG(gView.left + sx), LONG(gView.top + sy)}, dist, it.name, it.cat});
        }
    }

    gDzRadar.pts.clear();
    gDzRadar.ok = gSettings.dzRadar || gSettings.dzRadarScreen;
    if (gDzRadar.ok) {
        if (gDzRadarSnap.tUs != snap.tUs || gDzRadarSnap.ents.size() != snap.ents.size() || gDzRadarSnap.loot != snap.loot)
            gDzRadarSnap = snap;
        gDzRadar.me = me;
        gDzRadar.fwd = view.fwd;
        for (const DzSnap::E& ent : gDzRadarSnap.ents) gDzRadar.pts.push_back({ent.feet.x, ent.feet.z, ent.kind, &ent.name, ent.fwdX, ent.fwdZ, &ent.held});
        if (gSettings.dzLoot && gDzRadarSnap.loot)
            for (const DzLootItem& it : *gDzRadarSnap.loot) gDzRadar.pts.push_back({it.pos.x, it.pos.z, 10 + it.cat, &it.name});
    }
}

bool updateDayz(const char*& stage, int& count) {
    DzSnap snap;
    scanDayz(snap, stage);
    dz::Cam c;
    stage = "camera unreadable";
    if (!readDzCamera(snap.cam, c)) return true;
    float fakeProj[16] = {};
    fakeProj[0] = 1 / c.px;
    fakeProj[5] = 1 / c.py;
    stage = "placing view";
    if (gDma && !placeDmaView(fakeProj)) return true;
    const dz::Cam view = predictCam(c);
    const Vec3 me = snap.haveMe ? snap.me : c.pos;
    projectDz(snap, view, me);
    count = int(gBoxes.size());
    stage = "ok";
    return true;
}

void stopEsp();

#ifndef ICKZA_NO_DIAG
void runDiscoverSweep(const std::string& nameArg) {
    if (!gProc) { logLine("[!] name discovery: no target process"); return; }
    tel::Suppress noStats;
    static Image img;
    static bool imgOk = false;
    static DWORD imgPid = 0;
    static uintptr_t imgBase = 0;
    if (!imgOk || imgPid != gProc->pid() || imgBase != gProc->module().base) {
        img = Image();
        imgOk = false;
        std::string err;
        if (!img.load(*gProc, err)) { logLine("[!] name discovery: image load failed (%s)", err.c_str()); return; }
        imgOk = true;
        imgPid = gProc->pid();
        imgBase = gProc->module().base;
    }
    DiscoverConfig cfg;
    cfg.requireGlobal = false;
    cfg.chains = true;
    cfg.worldRva = dz::kWorld;
    cfg.tables = {{"near", dz::kNear}, {"far", dz::kFar}};
    cfg.localLink = dz::kLocalLink;
    cfg.localDeref = dz::kLocalDeref;
    cfg.localAdj = dz::kLocalAdj;
    const bool quick = nameArg == kQuickSweep;
    const std::string supplied = quick ? std::string() : nameArg;
    const std::string anchor = !supplied.empty() ? supplied : !gDzMeName.empty() ? gDzMeName : gSettings.dzNameAnchor;
    auto sink = [](const std::string& s) { logLine("[dump] %s", s.c_str()); };
    logLine("[*] name discovery: %s sweep starting (anchor=\"%s\"%s)", quick ? "quick" : "full", anchor.c_str(),
            !supplied.empty() ? " [supplied]" : !gDzMeName.empty() ? " [auto own-name]" : " [dz_name_anchor]");
    findNetRoster(*gProc, img, cfg, sink);
    findNameChain(*gProc, img, cfg, sink);
    if (quick) {
        logLine("[*] name discovery: quick sweep done");
        return;
    }
    if (anchor.size() >= 2) {
        findScoreboard(*gProc, img, cfg, anchor, sink);
        findMyName(*gProc, img, cfg, anchor, sink);
    } else {
        logLine("[!] name discovery: no anchor name (own-name chain empty, none supplied) - set dz_name_anchor=<gamertag> in Ickza.ini; skipped the name-anchored finders");
    }
    logLine("[*] name discovery: sweep done - results above are tagged [dump]");
}
#endif

void dzReadLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ULONGLONG lastAlive = GetTickCount64();
    ULONGLONG lastSpeed = lastAlive;
    constexpr ULONGLONG kSpeedMs = 30000;
    int miss = 0;
#ifndef ICKZA_NO_DIAG
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    ULONGLONG lastStat = lastAlive;
    int n = 0;
    double sum = 0, mx = 0;
#endif
    speed::quick(*gProc);
    logLine("[log] DMA speed test: %.1f MB/s", speed::gLinkMBps.load());
#ifndef ICKZA_NO_DIAG
    ULONGLONG lastLog = 0;
    const char* lastStage = "";
#endif
    while (gDzReadRun.load(std::memory_order_relaxed)) {
        const ULONGLONG t = GetTickCount64();
        if (t - lastAlive >= 500) {
            lastAlive = t;
            if (gProc->alive()) miss = 0;
            else if (++miss >= 4) {
                logLine("[log] game process gone on the target (%d checks)", miss);
                gDzGone = true;
                break;
            }
        }
        if (t - lastSpeed >= kSpeedMs) {
            lastSpeed = t;
            speed::quick(*gProc, 1);
        }
#ifndef ICKZA_NO_DIAG
        { std::string dn; if (tel::discoverRequested(dn)) runDiscoverSweep(dn); }
#endif
#ifndef ICKZA_NO_DIAG
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
#endif
        DzSnap s;
        const char* stage = "";
        scanDayz(s, stage);
        gProc->maintain();
#ifndef ICKZA_NO_DIAG
        QueryPerformanceCounter(&b);
        const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(qpf.QuadPart);
        sum += ms;
        mx = std::max(mx, ms);
        ++n;
        {
            static uint64_t lastDone = 0;
            const uint64_t done = qpcUs();
            tel::time(tel::T_SCAN, uint64_t(ms * 1000.0));
            if (lastDone) tel::time(tel::T_SCANGAP, done - lastDone);
            lastDone = done;
            tel::count(tel::C_SCANS);
            if (std::strcmp(stage, "ok") != 0) tel::count(tel::C_SCANS_BAD);
        }
        if (stage != lastStage) {
            logLine("[log] scan: %s", stage);
            lastStage = stage;
        }
#endif
        {
            std::lock_guard<std::mutex> lk(gDzSnapMx);
            gDzSnap = std::move(s);
        }
#ifndef ICKZA_NO_DIAG
        if (t - lastStat >= 1000) {
            gReadAvgMs = sum / std::max(1, n);
            gReadMaxMs = mx;
            sum = mx = 0;
            n = 0;
            lastStat = t;
        }
        if (t - lastLog >= 2000) {
            std::lock_guard<std::mutex> lk(gDzSnapMx);
            size_t rigged = 0, fallback = 0;
            for (const DzSnap::E& e : gDzSnap.ents) {
                if (e.kind != 1 && e.kind != 2) continue;
                (e.hasBones ? rigged : fallback)++;
            }
            logLine("[log] DayZ: %zu slots, %zu targets, %zu drawable, %d dead, loot %zu/%zu, bones %zu real / %zu fallback",
                    gDzSnap.slots, gDzSnap.targets, gDzSnap.ents.size(), gDzSnap.dead, gDzSnap.loot ? gDzSnap.loot->size() : 0, gDzSnap.lootSlots,
                    rigged, fallback);
            lastLog = t;
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void dzLootLoop() {
    markBackgroundReadThread();
    while (gDzReadRun.load(std::memory_order_relaxed)) {
        const ULONGLONG t0 = GetTickCount64();
        const uint64_t world = gDzWorldPub.load(std::memory_order_relaxed);
        if (gSettings.dzLoot && world) dzLootPass(world);
        const ULONGLONG spent = GetTickCount64() - t0;
        std::this_thread::sleep_for(std::chrono::milliseconds(spent < kLootPassMs ? kLootPassMs - spent : 1));
    }
}

void startDzThread() {
    if (gDzReadThread.joinable()) return;
    gDzGone = false;
    gDzReadRun = true;
    gDzWorldPub = 0;
    gDzLootThread = std::thread(dzLootLoop);
    gDzReadThread = std::thread(dzReadLoop);
    gDzCamThread = std::thread(dzCamLoop);
}

void stopDzThread() {
    gDzReadRun = false;
    if (gDzReadThread.joinable()) gDzReadThread.join();
    if (gDzCamThread.joinable()) gDzCamThread.join();
    if (gDzLootThread.joinable()) gDzLootThread.join();
}

bool drawDayzFrame() {
    if (gDzGone.exchange(false)) {
        logLine("[*] game closed");
        stopEsp();
        return false;
    }
    DzSnap snap;
    {
        std::lock_guard<std::mutex> lk(gDzSnapMx);
        snap = gDzSnap;
    }
    DzCamShare cs;
    {
        std::lock_guard<std::mutex> lk(gDzCamMx);
        cs = gDzCamShare;
    }
    const uint64_t nowUs = qpcUs();
    if (snap.tUs) tel::time(tel::T_SNAPAGE, nowUs - snap.tUs);
    const bool gotCam = snap.cam && cs.tUs && nowUs - cs.tUs < 250000;
    if (gotCam) {
        const dz::Cam& c = cs.c;
        float fakeProj[16] = {};
        fakeProj[0] = 1 / c.px;
        fakeProj[5] = 1 / c.py;
        placeDmaView(fakeProj);
        const dz::Cam view = dzViewCam(cs, nowUs);
        const Vec3 me = snap.haveMe ? snap.me : c.pos;
        gDzMeNameDraw = snap.meName;
        gDzMeHeldDraw = snap.meHeld;
        gDzOnlineDraw = snap.online;
        projectDz(snap, view, me);

#ifndef ICKZA_NO_DIAG
        gDzDbgBones.clear();
        if (gSettings.dzBoneIds && !snap.dbgBones.empty()) {
            const float w = float(gView.right - gView.left), h = float(gView.bottom - gView.top);
            for (int i = 0; i < int(snap.dbgBones.size()); ++i) {
                const Vec3& b = snap.dbgBones[i];
                float sx, sy;
                if (!std::isfinite(b.x) || !dz::toScreen(view, b, w, h, sx, sy)) continue;
                gDzDbgBones.push_back({{LONG(gView.left + sx), LONG(gView.top + sy)}, i});
            }
            static ULONGLONG lastBoneLog = 0;
            const ULONGLONG t = GetTickCount64();
            if (t - lastBoneLog >= 2000) {
                const Vec3& p0 = snap.dbgBones[0];
                logLine("[log] bones: kind %d, %zu read, %zu on-screen, bone0 %.2f %.2f %.2f", snap.dbgKind,
                        snap.dbgBones.size(), gDzDbgBones.size(), p0.x, p0.y, p0.z);
                lastBoneLog = t;
            }
        }
#endif
    } else {
        gBoxes.clear();
        gDzLootPts.clear();
        gDzRadar.ok = false;
    }
    static ULONGLONG lastFps = GetTickCount64();
    static int frames = 0;
    ++frames;
    const ULONGLONG now = GetTickCount64();
    if (now - lastFps >= 1000) {
        gFps = int(frames * 1000 / (now - lastFps));
        frames = 0;
        lastFps = now;
    }
#ifndef ICKZA_NO_DIAG
    static ULONGLONG lastRlog = lastFps;
    if (now - lastRlog >= 2000) {
        logLine("[log] render: %d fps, cam %.2f ms, paint %.2f ms, %d boxes", gFps, gDzCamMs.load(), gPaintMs, int(gBoxes.size()));
        lastRlog = now;
    }
#endif
    return true;
}

bool updateFrame(const char*& stage, int& count) {
    gBoxes.clear();
    stage = "game window not focused (overlay hidden)";
    if (!gDma && !placeOverlay()) return true;
    return updateDayz(stage, count);
}

struct BackBuffer {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    void release() {
        if (!dc) return;
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        dc = nullptr;
        w = h = 0;
    }
} gBack;

class GdiCanvas final : public Canvas {
public:
    explicit GdiCanvas(HDC dc) : dc_(dc) {
        SetBkMode(dc_, TRANSPARENT);
        oldBrush_ = SelectObject(dc_, GetStockObject(NULL_BRUSH));
        oldPen_ = SelectObject(dc_, GetStockObject(WHITE_PEN));
    }
    ~GdiCanvas() override {
        SelectObject(dc_, oldPen_);
        SelectObject(dc_, oldBrush_);
        for (const auto& p : pens_) DeleteObject(p.second);
        for (const auto& b : brushes_) DeleteObject(b.second);
    }
    void pen(COLORREF c, int width) override {
        const uint64_t key = (uint64_t(width) << 32) | c;
        auto it = pens_.find(key);
        if (it == pens_.end()) it = pens_.emplace(key, CreatePen(PS_SOLID, width, c)).first;
        SelectObject(dc_, it->second);
    }
    void line(int x0, int y0, int x1, int y1) override {
        MoveToEx(dc_, x0, y0, nullptr);
        LineTo(dc_, x1, y1);
    }
    void rect(int l, int t, int r, int b) override { Rectangle(dc_, l, t, r, b); }
    void ellipse(int l, int t, int r, int b) override { Ellipse(dc_, l, t, r, b); }
    void roundRect(int l, int t, int r, int b, int cw, int ch) override { RoundRect(dc_, l, t, r, b, cw, ch); }
    void polyline(const POINT* p, int n) override { Polyline(dc_, p, n); }
    void fill(const RECT& r, COLORREF c) override {
        auto it = brushes_.find(c);
        if (it == brushes_.end()) it = brushes_.emplace(c, CreateSolidBrush(c)).first;
        FillRect(dc_, &r, it->second);
    }
    void text(const char* s, const RECT& r, COLORREF c, UINT dt) override {
        SetTextColor(dc_, c);
        RECT rr = r;
        DrawTextA(dc_, s, -1, &rr, dt | DT_SINGLELINE | DT_NOCLIP);
    }

private:
    HDC dc_;
    HGDIOBJ oldBrush_, oldPen_;
    std::unordered_map<uint64_t, HPEN> pens_;
    std::unordered_map<COLORREF, HBRUSH> brushes_;
};

COLORREF dzKindColor(int kind, bool enemy) {
    switch (kind) {
        case 2: return RGB(255, 160, 40);
        case 3: return RGB(210, 180, 110);
        case 4: return RGB(170, 130, 255);
        default: return enemy ? RGB(255, 60, 60) : RGB(60, 220, 120);
    }
}

COLORREF dzLootColor(int cat) { return loot::info(cat).color; }

struct DzPlace {
    float x, z;
    int tier;
    std::string name;
};
const std::vector<DzPlace>& dzPlaces() {
    static std::string loadedFor;
    static int loadedGen = -1;
    static std::vector<DzPlace> places;
    if (loadedFor == gSettings.dzMap && loadedGen == gMapsGen) return places;
    loadedFor = gSettings.dzMap;
    loadedGen = gMapsGen;
    places.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, (exeDir() + "maps\\" + gSettings.dzMap + "_names.txt").c_str(), "r") || !f) return places;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        float x = 0, z = 0;
        char type[32] = {};
        int used = 0;
        if (sscanf_s(line, "%f %f %31s %n", &x, &z, type, unsigned(sizeof type), &used) < 3 || !used) continue;
        std::string name = line + used;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back();
        if (name.empty()) continue;
        const std::string t = type;
        const int tier = t == "Capital" ? 0 : t == "City" ? 1 : t == "Village" ? 2 : 3;
        places.push_back({x, z, tier, name});
        if (tier == 0)
            for (char& ch : places.back().name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    }
    std::fclose(f);
    logLine("[*] radar: %zu place names for %s", places.size(), gSettings.dzMap.c_str());
    return places;
}

using DzIcon = const char* const[7];
constexpr DzIcon kIconPerson = {"..###..", "..###..", "...#...", ".#####.", "#.###.#", "..#.#..", ".##.##."};
constexpr DzIcon kIconPaw = {".#.#.#.", ".#.#.#.", ".......", "..###..", ".#####.", ".#####.", "..###.."};
constexpr DzIcon kIconCar = {".......", "..###..", ".#####.", "#######", "#######", ".#...#.", "......."};
constexpr DzIcon kIconLoot[loot::Count] = {
    {"#######", "#..#..#", "#######", "#..#..#", "#..#..#", "#..#..#", "#######"},
    {".......", "......#", "#######", "###.#..", "#...#..", "#......", "......."},
    {".#...#.", "###.###", "###.###", "###.###", "###.###", "###.###", "......."},
    {"..###..", ".#...#.", "#..#..#", "#.###.#", "#..#..#", ".#...#.", "..###.."},
    {"......#", ".....##", "....##.", "...##..", ".###...", "##.....", "#......"},
    {"..##.#.", "..###..", ".#####.", "#######", "#######", ".#####.", "..###.."},
    {"..###..", "..###..", "#######", "#######", "#######", "..###..", "..###.."},
    {".#####.", "#######", "#.....#", "#.....#", "#######", "#######", ".#####."},
    {"...#...", "...#...", "..###..", ".#####.", ".#####.", ".#####.", "..###.."},
    {"....#.#", "....###", "...###.", "..###..", ".###...", "###....", ".#....."},
    {".##.##.", "#######", "#######", ".#####.", ".#####.", ".#####.", ".#####."},
    {"..###..", ".#...#.", "#######", "#######", "#.###.#", "#.###.#", "#######"},
    {"...#...", ".#####.", ".##.##.", "###.###", ".##.##.", ".#####.", "...#..."},
    {"...#...", "..###..", ".#####.", "#######", ".##.##.", ".##.##.", ".##.##."},
    {"...#...", "...#...", "#######", ".#####.", "..###..", ".##.##.", "#.....#"},
};

void drawDzIcon(Canvas& cv, const char* const* icon, int cx, int cy, int cell, COLORREF col) {
    const int x0 = cx - 7 * cell / 2, y0 = cy - 7 * cell / 2;
    for (int pass = 0; pass < 2; ++pass) {
        const int o = pass == 0 ? 1 : 0;
        const COLORREF c = pass == 0 ? RGB(1, 1, 1) : col;
        for (int r = 0; r < 7; ++r)
            for (int i = 0; i < 7;) {
                if (icon[r][i] != '#') { ++i; continue; }
                int j = i;
                while (j < 7 && icon[r][j] == '#') ++j;
                cv.fill({x0 + i * cell - o, y0 + r * cell - o, x0 + j * cell + o, y0 + (r + 1) * cell + o}, c);
                i = j;
            }
    }
}

float drawRadarView(Canvas& cv, const RECT& area, const Vec3& c, float R, bool full) {
    const int l = area.left, t = area.top, r = area.right, b = area.bottom;
    const float w = float(r - l), h = float(b - t);
    const float k = std::min(w, h) / 2.f / R;
    const float cx = l + w / 2.f, cy = t + h / 2.f;
    auto sx = [&](float x) { return cx + (x - c.x) * k; };
    auto sy = [&](float z) { return cy - (z - c.z) * k; };
    cv.fill(area, RGB(24, 26, 30));

    const float M = dzMapSize(gSettings.dzMap);
    const float x0 = c.x - w / 2.f / k, x1 = c.x + w / 2.f / k, z0 = c.z - h / 2.f / k, z1 = c.z + h / 2.f / k;
    const float cx0 = std::max(0.f, x0), cx1 = std::min(M, x1), cz0 = std::max(0.f, z0), cz1 = std::min(M, z1);
    if (cx1 > cx0 && cz1 > cz0) {
        const RECT dst{LONG(sx(cx0)), LONG(sy(cz1)), LONG(sx(cx1)), LONG(sy(cz0))};
        const std::string base = dzMapBase(gSettings.dzMap);
        if (!cv.image(base + ".png", cx0 / M, (M - cz1) / M, cx1 / M, (M - cz0) / M, dst, full ? 1.f : 0.85f))
            cv.image(base + ".jpg", cx0 / M, (M - cz1) / M, cx1 / M, (M - cz0) / M, dst, full ? 1.f : 0.85f);
    }

    if (full) {
        const float step = R > 400 ? 1000.f : 100.f;
        cv.pen(RGB(90, 95, 105), 1);
        for (float gx = std::ceil(x0 / step) * step; gx <= x1; gx += step) {
            const int px = int(sx(gx));
            cv.line(px, t, px, b);
            char lab[16];
            std::snprintf(lab, sizeof lab, step >= 1000 ? "%02.0f" : "%03.0f", gx / 100.f / (step >= 1000 ? 10.f : 1.f));
            cv.text(lab, {px + 3, b - 18, px + 60, b - 2}, RGB(170, 175, 185), DT_LEFT | DT_TOP);
        }
        for (float gz = std::ceil(z0 / step) * step; gz <= z1; gz += step) {
            const int py = int(sy(gz));
            cv.line(l, py, r, py);
            char lab[16];
            std::snprintf(lab, sizeof lab, step >= 1000 ? "%02.0f" : "%03.0f", (M - gz) / 100.f / (step >= 1000 ? 10.f : 1.f));
            cv.text(lab, {r - 40, py + 2, r - 2, py + 18}, RGB(170, 175, 185), DT_LEFT | DT_TOP);
        }
    } else {
        cv.pen(RGB(70, 75, 85), 1);
        cv.ellipse(int(cx - w / 4.f), int(cy - h / 4.f), int(cx + w / 4.f), int(cy + h / 4.f));
        cv.line(int(cx), t, int(cx), b);
        cv.line(l, int(cy), r, int(cy));
    }
    {
        const int maxTier = full ? (R <= 1200 ? 3 : R <= 4000 ? 2 : 1) : (R <= 600 ? 2 : 1);
        static const COLORREF kTierCol[] = {RGB(255, 255, 255), RGB(250, 245, 225), RGB(215, 215, 205), RGB(170, 190, 205)};
        for (const DzPlace& p : dzPlaces()) {
            if (p.tier > maxTier) continue;
            const int px = int(sx(p.x)), py = int(sy(p.z));
            if (px < l + 4 || px > r - 4 || py < t + 18 || py > b - 18) continue;
            const RECT box{px - 150, py - 8, px + 150, py + 8};
            cv.text(p.name.c_str(), {box.left + 1, box.top + 1, box.right + 1, box.bottom + 1}, RGB(1, 1, 1), DT_CENTER | DT_TOP);
            cv.text(p.name.c_str(), box, kTierCol[p.tier], DT_CENTER | DT_TOP);
        }
    }

    cv.pen(RGB(120, 125, 140), 1);
    cv.rect(l, t, r, b);
    cv.text("N", {l, t + 2, r, t + 18}, RGB(230, 230, 230), DT_CENTER | DT_TOP);

    const Vec3& me = gDzRadar.me;
    const float scale = std::clamp(gSettings.dzRadarMarker, 0.5f, 2.5f);
    const int entCell = std::max(1, int((full ? 2.f : 1.5f) * scale + 0.5f));
    const int lootCell = std::max(1, int((full ? 1.5f : 1.f) * scale + 0.5f));
    auto layer = [](int kind) { return kind >= 10 ? 0 : kind >= 3 ? 1 : kind == 2 ? 2 : 3; };
    for (int pass = 0; pass < 4; ++pass)
    for (const RadarPt& p : gDzRadar.pts) {
        if (layer(p.kind) != pass) continue;
        if (p.kind == 2 && !gSettings.dzInfected) continue;
        const int px = int(sx(p.x)), py = int(sy(p.z));
        const bool loot = p.kind >= 10;
        if (loot && !::loot::visible(gSettings.dzLootCats, p.kind - 10, p.name ? *p.name : std::string())) continue;
        const int cell = loot ? (p.kind == 10 + LC_Rare ? entCell : lootCell) : entCell;
        const int hs = 7 * cell / 2 + 1;
        if (px - hs < l || px + hs >= r || py - hs < t || py + hs >= b) continue;
        const COLORREF col = loot ? dzLootColor(p.kind - 10) : dzKindColor(p.kind, true);
        const char* const* icon = loot ? kIconLoot[std::clamp(p.kind - 10, 0, ::loot::Count - 1)]
                             : p.kind == 3 ? kIconPaw
                             : p.kind == 4 ? kIconCar
                                           : kIconPerson;
        drawDzIcon(cv, icon, px, py, cell, col);
        if (!gSettings.dzRadarNames || !p.name) continue;
        const float d = std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z));
        const bool label = loot ? (p.kind == 10 + LC_Rare || d <= float(std::max(25, gSettings.dzRadarLootLabel))) : p.kind != 2;
        if (!label) continue;
        char txt[96];
        if (loot) std::snprintf(txt, sizeof txt, "%s", p.name->c_str());
        else std::snprintf(txt, sizeof txt, "%s  %.0fm", p.name->empty() ? "?" : p.name->c_str(), d);
        cv.text(txt, {px + hs + 4, py - 8, px + hs + 300, py + 8}, RGB(1, 1, 1), DT_LEFT | DT_TOP);
        cv.text(txt, {px + hs + 3, py - 9, px + hs + 299, py + 7}, col, DT_LEFT | DT_TOP);
    }

    float fx = gDzRadar.fwd.x, fz = gDzRadar.fwd.z;
    const float fl = std::sqrt(fx * fx + fz * fz);
    const float ax = sx(me.x), ay = sy(me.z), as = full ? 2.f : 1.f;
    if (fl > 1e-3f && ax > l && ax < r && ay > t && ay < b) {
        fx /= fl;
        fz /= fl;
        const POINT tip{LONG(ax + fx * 12 * as), LONG(ay - fz * 12 * as)};
        const POINT lw{LONG(ax - (fx * 6 + fz * 6) * as), LONG(ay + (fz * 6 - fx * 6) * as)};
        const POINT rw{LONG(ax - (fx * 6 - fz * 6) * as), LONG(ay + (fz * 6 + fx * 6) * as)};
        const POINT arrow[4] = {lw, tip, rw, lw};
        cv.pen(RGB(1, 1, 1), 4);
        cv.polyline(arrow, 4);
        cv.pen(RGB(255, 255, 255), 2);
        cv.polyline(arrow, 4);
    }
    if (!gDzMeNameDraw.empty() && ax > l && ax < r && ay > t && ay < b) {
        const LONG tx = LONG(ax + 14 * as), ty = LONG(ay - 8);
        cv.text(gDzMeNameDraw.c_str(), {tx + 1, ty + 1, tx + 301, ty + 17}, RGB(1, 1, 1), DT_LEFT | DT_TOP);
        cv.text(gDzMeNameDraw.c_str(), {tx, ty, tx + 300, ty + 16}, RGB(255, 255, 255), DT_LEFT | DT_TOP);
    }
    return k;
}

void drawDzRadar(Canvas& cv) {
    const int S = std::clamp(gSettings.dzRadarSize, 120, 900);
    const RECT area{gView.right - S - 16, gView.top + 16, gView.right - 16, gView.top + 16 + S};
    drawRadarView(cv, area, gDzRadar.me, float(std::max(25, gSettings.dzRadarRange)), false);
    char scale[32];
    std::snprintf(scale, sizeof scale, "%dm", std::max(25, gSettings.dzRadarRange));
    cv.text(scale, {area.left + 5, area.bottom - 18, area.left + 120, area.bottom - 2}, RGB(200, 200, 200), DT_LEFT | DT_TOP);
}

void drawScene(Canvas& cv) {
    if (gDma && !gFull) {
        cv.pen(RGB(70, 70, 70), 1);
        cv.rect(gView.left, gView.top, gView.right, gView.bottom);
    }
    if (gSettings.fps) {
        char fps[24];
        std::snprintf(fps, sizeof fps, "%d FPS", gFps);
        cv.text(fps, {gView.left + 7, gView.top + 7, gView.right, gView.top + 27}, RGB(1, 1, 1), DT_LEFT | DT_TOP);
        cv.text(fps, {gView.left + 6, gView.top + 6, gView.right, gView.top + 26}, RGB(255, 220, 80), DT_LEFT | DT_TOP);
    }
    for (const auto& b : gBoxes) {
        const COLORREF col = dzKindColor(b.kind, b.enemy);
        const int l = int(b.x), t = int(b.y), r = int(b.x + b.w), btm = int(b.y + b.h);
        const auto& opt = gSettings;
        if (b.kind >= 3) {
            cv.pen(RGB(1, 1, 1), 3);
            cv.rect(l, t, r, btm);
            cv.pen(col, 1);
            cv.rect(l, t, r, btm);
            if (opt.names) cv.text(b.e.name.c_str(), {l - 80, t - 16, r + 80, t - 1}, col, DT_CENTER | DT_BOTTOM);
            if (opt.info) {
                char info[16];
                std::snprintf(info, sizeof info, "%.0fm", b.dist);
                cv.text(info, {l - 80, btm + 1, r + 80, btm + 16}, col, DT_CENTER | DT_TOP);
            }
            continue;
        }

        auto outlined = [&](auto&& draw) {
            cv.pen(RGB(1, 1, 1), 3);
            draw();
            cv.pen(col, 1);
            draw();
        };
        if (opt.box) outlined([&] { cv.rect(l, t, r, btm); });
        if (opt.skeleton && b.skel.ok[DJ_Pelvis] && b.skel.ok[DJ_Neck]) {
            const Skeleton& s = b.skel;
            outlined([&] {
                for (const auto& bone : dz::kDzSkelBones) {
                    if (!s.ok[bone.first] || !s.ok[bone.second]) continue;
                    cv.line(s.pt[bone.first].x, s.pt[bone.first].y, s.pt[bone.second].x, s.pt[bone.second].y);
                }
                if (s.headRadius && s.ok[DJ_Head]) {
                    const POINT c = s.pt[DJ_Head];
                    cv.ellipse(c.x - s.headRadius, c.y - s.headRadius, c.x + s.headRadius, c.y + s.headRadius);
                }
            });
        } else if (opt.skeleton) {
            const int bw = r - l, bh = btm - t;
            auto px = [&](int j) { return POINT{LONG(l + kStick[j].nx * bw), LONG(t + kStick[j].ny * bh)}; };
            const int hr = std::max(2, bw / 6);
            outlined([&] {
                for (const auto& bone : kStickBones) {
                    const POINT a = px(bone.first), c = px(bone.second);
                    cv.line(a.x, a.y, c.x, c.y);
                }
                POINT hd = px(0);
                hd.y += hr;
                cv.ellipse(hd.x - hr, hd.y - hr, hd.x + hr, hd.y + hr);
            });
        }

        if (opt.names) cv.text(b.e.name.c_str(), {l - 80, t - 16, r + 80, t - 1}, col, DT_CENTER | DT_BOTTOM);
        if (opt.info) {
            char info[64];
            std::snprintf(info, sizeof info, b.weapon.empty() ? "%.0fm" : "%.0fm  %s", b.dist, b.weapon.c_str());
            cv.text(info, {l - 80, btm + 1, r + 80, btm + 16}, col, DT_CENTER | DT_TOP);
        }
    }

    if (gSettings.dzLoot && !gDzLootPts.empty()) {
        for (const auto& lp : gDzLootPts) {
            const COLORREF lc = dzLootColor(lp.cat);
            const int x = lp.pt.x, y = lp.pt.y, r = lp.cat == LC_Rare ? 6 : 4;
            const POINT d[5] = {{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}, {x, y - r}};
            cv.pen(RGB(1, 1, 1), 3);
            cv.polyline(d, 5);
            cv.pen(lc, lp.cat == LC_Rare ? 2 : 1);
            cv.polyline(d, 5);
            if (!lp.name.empty())
                cv.text(lp.name.c_str(), {x - 90, y - r - 16, x + 90, y - r - 2}, lc, DT_CENTER | DT_BOTTOM);
            if (gSettings.info) {
                char info[16];
                std::snprintf(info, sizeof info, "%.0fm", lp.dist);
                cv.text(info, {x - 40, y + r + 1, x + 40, y + r + 15}, lc, DT_CENTER | DT_TOP);
            }
        }
    }

    if (gDzRadar.ok && gSettings.dzRadar) drawDzRadar(cv);

#ifndef ICKZA_NO_DIAG
    if (gSettings.dzBoneIds && !gDzDbgBones.empty()) {
        cv.pen(RGB(0, 255, 255), 1);
        for (const auto& b : gDzDbgBones) {
            cv.ellipse(b.pt.x - 2, b.pt.y - 2, b.pt.x + 2, b.pt.y + 2);
            char num[8];
            std::snprintf(num, sizeof num, "%d", b.idx);
            cv.text(num, {b.pt.x + 3, b.pt.y - 7, b.pt.x + 40, b.pt.y + 8}, RGB(0, 255, 255), DT_LEFT | DT_TOP);
        }
    }
#endif

    if (gSettings.crosshair) {
        const int cx = (gView.left + gView.right) / 2, cy = (gView.top + gView.bottom) / 2;
        const int arm = std::clamp(gSettings.crosshairSize, 4, 40), gap = std::max(2, arm / 3);
        const COLORREF col = RGB(80, 255, 140);
        for (int pass = 0; pass < 2; ++pass) {
            const int o = pass == 0 ? 1 : 0;
            const COLORREF c = pass == 0 ? RGB(1, 1, 1) : col;
            cv.fill({cx - arm - o, cy - o, cx - gap + 1 + o, cy + 1 + o}, c);
            cv.fill({cx + gap - o, cy - o, cx + arm + 1 + o, cy + 1 + o}, c);
            cv.fill({cx - o, cy - arm - o, cx + 1 + o, cy - gap + 1 + o}, c);
            cv.fill({cx - o, cy + gap - o, cx + 1 + o, cy + arm + 1 + o}, c);
            cv.fill({cx - o, cy - o, cx + 1 + o, cy + 1 + o}, c);
        }
    }
}

void paint(HDC hdc, int w, int h) {
    if (!gBack.dc || gBack.w != w || gBack.h != h) {
        gBack.release();
        gBack.dc = CreateCompatibleDC(hdc);
        gBack.bmp = CreateCompatibleBitmap(hdc, w, h);
        gBack.old = SelectObject(gBack.dc, gBack.bmp);
        gBack.w = w;
        gBack.h = h;
    }
    HDC mem = gBack.dc;
    RECT all{0, 0, w, h};
    HBRUSH key = CreateSolidBrush(kKey);
    FillRect(mem, &all, key);
    DeleteObject(key);
    {
        GdiCanvas cv(mem);
        drawScene(cv);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
}

enum class Run { Idle, Attaching, Running } gRun = Run::Idle;
unsigned gGen = 0;
std::shared_ptr<std::atomic<bool>> gCancel;
HWND gPanel = nullptr, gCtl = nullptr;
RECT gScreen{};
constexpr UINT WM_ATTACHED = WM_APP + 10;

struct AttachResult {
    unsigned gen = 0;
    std::unique_ptr<Process> proc;
    std::map<uint64_t, int> dzKinds;
    std::map<uint64_t, int> dzLootVts;
    std::string err;
};

constexpr const char* kGameExe = "DayZ_x64.exe";

PanelInfo espInfo() {
    PanelInfo in;
    if (gRun != Run::Running || !gProc) return in;
    in.pid = gProc->pid();
    {
        std::lock_guard<std::mutex> lk(gDzServerMx);
        in.server = gDzServer;
    }
    if (gDma) in.mbps = speed::gLinkMBps;
    return in;
}

void destroyRadarScreen();

void stopEsp() {
    ++gGen;
    if (gCancel) *gCancel = true;
    gCancel.reset();
    destroyRadarScreen();
    stopDzThread();
    gGpu.reset();
    gGpuFails = 0;
    if (gOverlay) DestroyWindow(gOverlay);
    gOverlay = nullptr;
    gGame = nullptr;
    gProc.reset();
    { std::lock_guard<std::mutex> lk(gDzSnapMx); gDzSnap = DzSnap{}; }
    gDzKinds.clear();
    gDzLootVts.clear();
    gDzLootCat.clear();
    gDzEntHeld.clear();
    gDzItemName.clear();
    gDzRoster.clear();
    gDzEntNetId.clear();
    gDzRadar = DzRadar{};
    gDzEntKind.clear();
    gDzEntPos.clear();
    gDzEntVs.clear();
    gDzEntVel.clear();
    gDzEntPosUs.clear();
    gDzEntFwd.clear();
    gDzEntName.clear();
    gDzEntChain.clear();
    gDzEntJointOfs.clear();
    gDzEntReadUs.clear();
    gDzEntBoneUs.clear();
    gDzEntDead.clear();
    { std::lock_guard<std::mutex> lk(gDzCamMx); gDzCamShare = DzCamShare{}; }
    gDzLootVs.clear();
    gDzLootPos.clear();
    gDzLootName.clear();
    gDzLootPts.clear();
    { std::lock_guard<std::mutex> lk(gDzLootMx); gDzLootList.reset(); gDzLootSlotsPub = 0; }
    gDzWorld = 0;
    gBoxes.clear();
    gBack.release();
    gFps = 0;
    if (gRun != Run::Idle) logLine("[*] stopped");
    gRun = Run::Idle;
    { std::lock_guard<std::mutex> lk(gDzServerMx); gDzServer.clear(); }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (!(gGpu && hwnd == gOverlay)) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                paint(hdc, rc.right, rc.bottom);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: stopEsp(); return 0;
#ifndef ICKZA_NO_DIAG
        case WM_DISPLAYCHANGE: logLine("[log] display change: %ux%u", LOWORD(lp), HIWORD(lp)); break;
        case WM_DPICHANGED: logLine("[log] window moved to a monitor with dpi %u", LOWORD(wp)); break;
#endif
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

ULONGLONG gRadarTick = 0;

constexpr ULONGLONG kSaveDelayMs = 500;
ULONGLONG gSaveDue = 0;
void saveSettingsSoon() { gSaveDue = GetTickCount64() + kSaveDelayMs; }
void flushSettings(bool now = false) {
    if (!gSaveDue || (!now && GetTickCount64() < gSaveDue)) return;
    gSaveDue = 0;
    saveSettings(gSettings);
}

void destroyRadarScreen() { radar::destroy(); }

#ifndef ICKZA_NO_DIAG
const bool gRadarLogWired = [] {
    radar::setLog([](const char* s) { logLine("%s", s); });
    mapdl::setLog([](const char* s) { logLine("%s", s); });
    return true;
}();
#endif

void syncRadarScreen() {
    static bool f8Was = false;
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8 && !f8Was) {
        gSettings.dzRadarScreen = !gSettings.dzRadarScreen;
        saveSettings(gSettings);
        if (gPanel) InvalidateRect(gPanel, nullptr, FALSE);
    }
    f8Was = f8;

    dzMapDownloads();

    {
        std::string detected;
        {
            std::lock_guard<std::mutex> lk(gDzMapMx);
            detected.swap(gDzMapDetected);
        }
        if (!detected.empty() && gSettings.dzMapAuto && gSettings.dzMap != detected) {
            gSettings.dzMap = detected;
            saveSettings(gSettings);
        }
    }

    const bool wantShow = gSettings.dzRadarScreen;
    if (!wantShow && !radar::visible()) return;
    if (wantShow && radar::visible()) {
        const ULONGLONG now = GetTickCount64();
        if (now - gRadarTick < 33) return;
        gRadarTick = now;
    }

    RECT tgt{};
    const RECT* ptgt = nullptr;
    for (const auto& d : listDisplays())
        if (!EqualRect(&d.rc, &gScreen)) {
            tgt = d.rc;
            ptgt = &tgt;
            break;
        }

    radar::Frame fr;
    fr.ok = gDzRadar.ok;
    if (fr.ok) {
        fr.meX = gDzRadar.me.x;
        fr.meZ = gDzRadar.me.z;
        fr.fwdX = gDzRadar.fwd.x;
        fr.fwdZ = gDzRadar.fwd.z;
        fr.meName = gDzMeNameDraw;
        fr.mapBase = dzMapBase(gSettings.dzMap);
        fr.mapSize = dzMapSize(gSettings.dzMap);
        const Vec3& me = gDzRadar.me;

        static const COLORREF kTierCol[] = {RGB(255, 255, 255), RGB(250, 245, 225), RGB(215, 215, 205), RGB(170, 190, 205)};
        for (const DzPlace& p : dzPlaces()) fr.places.push_back({p.x, p.z, p.tier, kTierCol[p.tier], p.name});

        for (const RadarPt& p : gDzRadar.pts) {
            const bool isLoot = p.kind >= 10;
            radar::Dot dot;
            dot.x = p.x;
            dot.z = p.z;
            if (isLoot) {
                dot.icon = radar::Icon::Loot;
                dot.cat = p.kind - 10;
            } else {
                dot.color = dzKindColor(p.kind, true);
                dot.fwdX = p.fwdX;
                dot.fwdZ = p.fwdZ;
                switch (p.kind) {
                    case 2: dot.icon = radar::Icon::Infected; break;
                    case 3: dot.icon = radar::Icon::Animal; break;
                    case 4: dot.icon = radar::Icon::Vehicle; break;
                    default: dot.icon = radar::Icon::Survivor; break;
                }
            }
            if (p.name && !(isLoot ? p.name->empty() : p.kind == 2)) {
                char txt[160];
                if (isLoot) {
                    std::snprintf(txt, sizeof txt, "%s", p.name->c_str());
                } else {
                    const float d = std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z));
                    const bool held = p.held && !p.held->empty() && gSettings.dzHeld;
                    std::snprintf(txt, sizeof txt, held ? "%s  %.0fm\n%s" : "%s  %.0fm", p.name->empty() ? "?" : p.name->c_str(), d,
                                  held ? p.held->c_str() : "");
                }
                dot.label = txt;
            }
            fr.dots.push_back(std::move(dot));
        }

        if (gDzOnlineDraw) {
            std::unordered_map<std::string, float> nearby;
            for (const RadarPt& p : gDzRadar.pts)
                if (p.kind == 1 && p.name && !p.name->empty()) {
                    const float d = std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z));
                    const auto it = nearby.find(*p.name);
                    if (it == nearby.end() || d < it->second) nearby[*p.name] = d;
                }
            for (const std::string& nm : *gDzOnlineDraw) {
                const auto it = nearby.find(nm);
                fr.online.push_back({nm, it != nearby.end() ? it->second : -1.f, nm == fr.meName});
            }
        }
    }

    radar::Config cfg;
    cfg.loot = gSettings.dzLoot;
    cfg.lootMask = gSettings.dzLootCats & loot::kAll;
    cfg.filter = loot::filter();
    cfg.headings = gSettings.dzRadarHeadings;
    cfg.headingLen = gSettings.dzRadarHeadingLen;
    cfg.esp.skeleton = gSettings.skeleton;
    cfg.esp.box = gSettings.box;
    cfg.esp.names = gSettings.names;
    cfg.esp.distance = gSettings.info;
    cfg.esp.fps = gSettings.fps;
    cfg.esp.uncapped = gSettings.uncapped;
    cfg.esp.held = gSettings.dzHeld;
    cfg.esp.valuables = gSettings.dzLootValuables;
    cfg.esp.minimap = gSettings.dzRadar;
    cfg.esp.range = gSettings.dzRange;
    cfg.esp.crosshair = gSettings.crosshair;
    cfg.esp.crosshairSize = gSettings.crosshairSize;
    cfg.animals = gSettings.dzAnimals;
    cfg.vehicles = gSettings.dzVehicles;
    cfg.infected = gSettings.dzInfected;
    cfg.names = gSettings.dzRadarNames;
    cfg.lootLabelRange = gSettings.dzRadarLootLabel;
    cfg.markerScale = gSettings.dzRadarMarker;
    cfg.mapAuto = gSettings.dzMapAuto;
    cfg.map = gSettings.dzMap;
    cfg.mapTopo = gSettings.dzMapTopo;
    {
        std::lock_guard<std::mutex> lk(gDzMapMx);
        cfg.mapDetected = gDzMapSeen.empty() ? std::string() : mapdl::label(gDzMapSeen);
    }
    cfg.mapHigh = gSettings.dzMapHigh;
    cfg.mapList = radarMapList();
    {
        const mapdl::Status st = mapdl::status();
        cfg.dl.busy = st.busy;
        cfg.dl.label = mapdl::label(st.id);
        cfg.dl.phase = st.phase;
        cfg.dl.done = st.done;
        cfg.dl.total = st.total;
        cfg.dl.mbps = float(st.mbps);
        if (st.id == gSettings.dzMap && st.topo == gSettings.dzMapTopo) cfg.dl.error = st.error;
    }

    bool want = wantShow;
    int zoom = std::max(25, gSettings.dzRadarZoom);
    if (!radar::sync(want, zoom, cfg, fr, ptgt)) {
        gSettings.dzRadarScreen = false;
        saveSettings(gSettings);
        if (gPanel) InvalidateRect(gPanel, nullptr, FALSE);
        return;
    }
    const bool zoomed = zoom != gSettings.dzRadarZoom;
    gSettings.dzRadarZoom = zoom;
    if (zoomed) saveSettingsSoon();
    if (cfg.cancelReq) mapdl::cancel();
    if (!cfg.downloadReq.empty()) dzMapDownloadNow(cfg.downloadReq);
    if (cfg.showSetup && gPanel) {
        ShowWindow(gPanel, SW_RESTORE);
        SetWindowPos(gPanel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(gPanel);
    }
    if (cfg.changed) {
        gSettings.dzLoot = cfg.loot;
        gSettings.dzLootCats = cfg.lootMask;
        loot::setFilter(cfg.filter);
        const loot::Filter fl = loot::filter();
        gSettings.dzFilterMode = fl.mode;
        gSettings.dzFilterList = loot::serializeList(fl.list);
        gSettings.dzRadarHeadings = cfg.headings;
        gSettings.dzRadarHeadingLen = cfg.headingLen;
        gSettings.skeleton = cfg.esp.skeleton;
        gSettings.box = cfg.esp.box;
        gSettings.names = cfg.esp.names;
        gSettings.info = cfg.esp.distance;
        gSettings.fps = cfg.esp.fps;
        gSettings.uncapped = cfg.esp.uncapped;
        gSettings.dzHeld = cfg.esp.held;
        gSettings.dzLootValuables = cfg.esp.valuables;
        gSettings.dzRadar = cfg.esp.minimap;
        gSettings.dzRange = cfg.esp.range;
        gSettings.crosshair = cfg.esp.crosshair;
        gSettings.crosshairSize = cfg.esp.crosshairSize;
        gSettings.dzAnimals = cfg.animals;
        gSettings.dzVehicles = cfg.vehicles;
        gSettings.dzInfected = cfg.infected;
        gSettings.dzRadarNames = cfg.names;
        gSettings.dzRadarLootLabel = cfg.lootLabelRange;
        gSettings.dzRadarMarker = cfg.markerScale;
        if (cfg.mapAuto && !gSettings.dzMapAuto) gDzMapRedetect = true;
        gSettings.dzMapAuto = cfg.mapAuto;
        gSettings.dzMap = cfg.map;
        gSettings.dzMapTopo = cfg.mapTopo;
        gSettings.dzMapHigh = cfg.mapHigh;
        saveSettingsSoon();
    }
    if (wantShow && !want) {
        gSettings.dzRadarScreen = false;
        saveSettings(gSettings);
        if (gPanel) InvalidateRect(gPanel, nullptr, FALSE);
    }
}

void runFrame() {
    if (GetAsyncKeyState(VK_END) & 0x8000) {
        PostMessage(gPanel, WM_CLOSE, 0, 0);
        return;
    }
    const bool uncapped = gSettings.uncapped;
    if (gGpu && !uncapped) gGpu->waitForFrame();
    {
        static uint64_t lastFrameUs = 0;
        const uint64_t nowUs = qpcUs();
        if (lastFrameUs) tel::time(tel::T_FRAME, nowUs - lastFrameUs);
        lastFrameUs = nowUs;
        tel::count(tel::C_FRAMES);
    }
    if (gDma) {
        if (!drawDayzFrame()) return;
    } else if (!update()) {
        logLine("[*] game closed");
        stopEsp();
        return;
    }
    if (gGpu) {
        static bool hadMinimap = false;
        static std::string mapWas;
        if ((hadMinimap && !gSettings.dzRadar) || (hadMinimap && mapWas != gSettings.dzMap)) {
            gGpu->releaseImages();
            logLine("[log] minimap: map image released");
        }
        hadMinimap = gSettings.dzRadar;
        mapWas = gSettings.dzMap;
    }
#ifndef ICKZA_NO_DIAG
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
#endif
    bool gpuOk = true;
    if (gGpu) {
        Canvas* cv = gGpu->begin(kKey);
        gpuOk = cv != nullptr;
        if (gpuOk) {
            drawScene(*cv);
            gpuOk = gGpu->end();
        }
    } else {
        RECT rc;
        GetClientRect(gOverlay, &rc);
        HDC hdc = GetDC(gOverlay);
        paint(hdc, rc.right, rc.bottom);
        ReleaseDC(gOverlay, hdc);
    }
    ValidateRect(gOverlay, nullptr);
#ifndef ICKZA_NO_DIAG
    QueryPerformanceCounter(&t1);
#endif
    if (gGpu) {
        const uint64_t p0 = qpcUs();
        gpuOk = gpuOk && gGpu->present(!uncapped);
        tel::time(tel::T_PRESENT, qpcUs() - p0);
        if (!gpuOk) {
            gGpu.reset();
            std::string err;
            if (++gGpuFails <= 3) gGpu = GpuRenderer::create(gOverlay, err);
            logLine(gGpu ? "[!] GPU renderer lost; recreated" : "[!] GPU renderer lost; using GDI (%s)", err.c_str());
        }
    }
#ifndef ICKZA_NO_DIAG
    const double paintMs = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    gPaintMs = gPaintMs * 0.9 + paintMs * 0.1;
    tel::time(tel::T_PAINT, uint64_t(paintMs * 1000.0));
#endif
    {
        static uint64_t lastRadarUs = 0;
        const uint64_t nowUs = qpcUs();
        if (!uncapped || nowUs - lastRadarUs >= 6000) {
            lastRadarUs = nowUs;
            syncRadarScreen();
        }
    }
}

bool createOverlay() {
    const HINSTANCE inst = GetModuleHandleA(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc{};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = "ickza_fullscreen";
        RegisterClassA(&wc);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "ickza_overlay";
        RegisterClassA(&wc);
        registered = true;
    }
    if (gFull) {
        gOverlay = CreateWindowExA(WS_EX_TOPMOST, "ickza_fullscreen", "Ickza - DMA view (END quits)", WS_POPUP | WS_VISIBLE, gScreen.left,
                                   gScreen.top, gScreen.right - gScreen.left, gScreen.bottom - gScreen.top, nullptr, nullptr, inst, nullptr);
        if (gOverlay)
            SetWindowPos(gOverlay, HWND_TOPMOST, gScreen.left, gScreen.top, gScreen.right - gScreen.left, gScreen.bottom - gScreen.top,
                         SWP_NOACTIVATE);
    } else if (gDma) {
        gOverlay = CreateWindowExA(0, "ickza_overlay", "Ickza - DMA view (END quits)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                   CW_USEDEFAULT, 1280, 760, nullptr, nullptr, inst, nullptr);
    } else {
        gOverlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "ickza_overlay",
                                   "Ickza", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
        if (gOverlay) SetLayeredWindowAttributes(gOverlay, kKey, 0, LWA_COLORKEY);
    }
    if (!gOverlay) {
        logLine("[-] could not create the window (error %lu)", GetLastError());
        return false;
    }
    if (gDma) logWindow("window created", gOverlay);
    if (gFull) {
        std::string err = "disabled by --gdi";
        if (!gForceGdi) gGpu = GpuRenderer::create(gOverlay, err);
        if (gGpu) logLine("[+] renderer: %s", gGpu->describe().c_str());
        else logLine("[!] renderer: GDI (%s)", err.c_str());
    }
    return true;
}

void startEsp() {
    if (gRun != Run::Idle) return;
    gDma = gSettings.dma;
    gFull = gDma && gSettings.fullscreen;
#ifndef ICKZA_NO_DIAG
    const EspSettings& o = gSettings;
    logLine("[log] start: %s%s, draw: skeleton=%d box=%d names=%d info=%d fps=%d", gDma ? "DMA" : "this PC", gFull ? " fullscreen" : "",
            o.skeleton, o.box, o.names, o.info, o.fps);
#endif
    if (gFull) {
        const auto displays = listDisplays();
#ifndef ICKZA_NO_DIAG
        for (const auto& d : displays) {
            DEVMODEA dm{};
            dm.dmSize = sizeof dm;
            const DWORD hz = EnumDisplaySettingsA(d.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
            logLine("[log] display %d: %ldx%ld at %ld,%ld @ %lu Hz%s  (%s)", d.number, d.width(), d.height(), d.rc.left, d.rc.top, hz,
                    d.primary ? " (primary)" : "", d.device.c_str());
        }
#endif
        const DisplayInfo* pick = pickDisplay(displays, gSettings);
        if (!pick) {
            logLine("[-] no display to go fullscreen on");
            return;
        }
        gScreen = pick->rc;
        logLine("[+] fullscreen on display %d (%ldx%ld)", pick->number, pick->width(), pick->height());
#ifndef ICKZA_NO_DIAG
        DEVMODEA pdm{};
        pdm.dmSize = sizeof pdm;
        if (EnumDisplaySettingsA(pick->device.c_str(), ENUM_CURRENT_SETTINGS, &pdm)) {
            std::vector<DWORD> rates;
            DEVMODEA m{};
            m.dmSize = sizeof m;
            for (DWORD i = 0; EnumDisplaySettingsA(pick->device.c_str(), i, &m); ++i)
                if (m.dmPelsWidth == pdm.dmPelsWidth && m.dmPelsHeight == pdm.dmPelsHeight &&
                    std::find(rates.begin(), rates.end(), m.dmDisplayFrequency) == rates.end())
                    rates.push_back(m.dmDisplayFrequency);
            std::sort(rates.begin(), rates.end());
            std::string list;
            for (DWORD r : rates) list += (list.empty() ? "" : ", ") + std::to_string(r);
            logLine("[log] display %d refresh rates at %lux%lu: %s Hz (current %lu)", pick->number, pdm.dmPelsWidth, pdm.dmPelsHeight, list.c_str(),
                    pdm.dmDisplayFrequency);
        }
#endif
    }

    gRun = Run::Attaching;
    const unsigned gen = ++gGen;
    auto cancel = gCancel = std::make_shared<std::atomic<bool>>(false);
    logLine(gDma ? "[*] waiting for DayZ_x64.exe on the target PC over DMA (offline mode, spawned in)"
                 : "[*] waiting for DayZ_x64.exe (offline mode, spawned in)");
    std::thread([gen, cancel, dma = gDma, device = gSettings.device, dmaDir = gSettings.dmaDir, ctl = gCtl] {
        auto res = std::make_unique<AttachResult>();
        res->gen = gen;
        res->proc = dma ? makeDmaProcess(device, dmaDir, true) : makeLocalProcess();
        std::string err;
#ifndef ICKZA_NO_DIAG
        std::string lastErr;
#endif
        while (!res->proc->attach(kGameExe, err)) {
            if (*cancel) return;
            if (err.rfind("fatal:", 0) == 0) {
                res->err = err.substr(7);
                res->proc.reset();
                PostMessage(ctl, WM_ATTACHED, 0, reinterpret_cast<LPARAM>(res.release()));
                return;
            }
#ifndef ICKZA_NO_DIAG
            if (dma && err != lastErr) logLine("[*] %s", (lastErr = err).c_str());
#endif
            Sleep(500);
        }
        Image img;
        std::vector<VTableInfo> vts;
        const bool loaded = img.load(*res->proc, err);
        for (const auto& [cls, kind] : {std::pair<const char*, int>{"DayZPlayer", 1}, {"DayZInfected", 2}}) {
            if (loaded && findVTables(img, cls, vts, err)) {
                for (const auto& v : vts) res->dzKinds[static_cast<uint64_t>(img.base) + v.rva] = kind;
                logLine("[+] %s: %zu vtable(s)", cls, vts.size());
            } else {
                const uint64_t rva = kind == 1 ? 0x12E2160 : 0x12FBF70;
                res->dzKinds[res->proc->module().base + rva] = kind;
                logLine("[!] %s RTTI unavailable (%s); using RVA 0x%llX from DayZ 1.30 Main", cls, err.c_str(),
                        static_cast<unsigned long long>(rva));
            }
        }
        if (loaded) {
            for (const auto& [cls, kind] : {std::pair<const char*, int>{"DayZAnimal", 3}, {"Transport", 4}}) {
                if (!findVTables(img, cls, vts, err)) { logLine("[!] %s RTTI unavailable (%s)", cls, err.c_str()); continue; }
                for (const auto& v : vts) res->dzKinds.emplace(static_cast<uint64_t>(img.base) + v.rva, kind);
                logLine("[+] %s: %zu vtable(s)", cls, vts.size());
            }
            for (const auto& [cls, cat] : {std::pair<const char*, int>{"Weapon", LC_Weapon}, {"Magazine", LC_Ammo}, {"Clothing_Base", LC_Clothing}}) {
                if (!findVTables(img, cls, vts, err)) { logLine("[!] %s RTTI unavailable (%s)", cls, err.c_str()); continue; }
                for (const auto& v : vts) res->dzLootVts.emplace(static_cast<uint64_t>(img.base) + v.rva, cat);
                logLine("[+] loot class %s: %zu vtable(s)", cls, vts.size());
            }
        }
        if (*cancel) return;
        PostMessage(ctl, WM_ATTACHED, 0, reinterpret_cast<LPARAM>(res.release()));
    }).detach();
}

LRESULT CALLBACK ctlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ATTACHED) {
        std::unique_ptr<AttachResult> r(reinterpret_cast<AttachResult*>(lp));
        if (r->gen != gGen || gRun != Run::Attaching) return 0;
        if (!r->proc) {
            logLine("[-] %s", r->err.c_str());
            stopEsp();
            MessageBoxA(gPanel, r->err.c_str(), "Ickza", MB_OK | MB_ICONERROR);
            return 0;
        }
        gProc = std::move(r->proc);
        gDzKinds = std::move(r->dzKinds);
        gDzLootVts = std::move(r->dzLootVts);
        dzLoadRare();
        const size_t overrides = loot::loadOverrides(exeDir() + "dayz_loot_cats.txt");
        logLine("[*] dayz_loot_cats.txt: %zu category override(s)", overrides);
        gCancel.reset();
        logLine("[+] attached: pid %lu, base 0x%llX. END quits.", static_cast<unsigned long>(gProc->pid()),
                static_cast<unsigned long long>(gProc->module().base));
        if (!createOverlay()) {
            logLine("[-] could not create the overlay window");
            stopEsp();
            return 0;
        }
        gRun = Run::Running;
        if (gDma) startDzThread();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

#ifndef ICKZA_NO_DIAG
std::string exeDir0() {
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    return std::string(self).substr(0, std::string(self).find_last_of("\\/"));
}

bool updatePending() {
    static ULONGLONG last = GetTickCount64();
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return false;
    last = now;
    const std::string pending = exeDir0() + "\\Ickza_pending.exe";
    if (GetFileAttributesA(pending.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const HANDLE h = CreateFileA(pending.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    logLine("[log] update pending - exiting to relaunch into the new build");
    return true;
}

int swapAndRelaunch() {
    char selfBuf[MAX_PATH];
    GetModuleFileNameA(nullptr, selfBuf, MAX_PATH);
    const std::string self = selfBuf, dir = exeDir0();
    const std::string old = dir + "\\Ickza_old.exe", pending = dir + "\\Ickza_pending.exe";
    const auto moveRetry = [](const std::string& from, const std::string& to) {
        for (int i = 0; i < 40; ++i) {
            if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
            Sleep(250);
        }
        return false;
    };
    if (moveRetry(self, old)) {
        if (moveRetry(pending, self)) {
            logLine("[log] swapped in the new build");
        } else {
            logLine("[!] update: could not move Ickza_pending.exe into place (error %lu) - restarting the old build",
                    GetLastError());
            moveRetry(old, self);
        }
    } else {
        logLine("[!] update: could not rename the running exe (error %lu) - restarting it", GetLastError());
    }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, GetCommandLineW(), nullptr, nullptr, FALSE, 0, nullptr, std::wstring(dir.begin(), dir.end()).c_str(),
                        &si, &pi)) {
        logLine("[!] update: could not relaunch (error %lu)", GetLastError());
        return 10;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logLine("[log] relaunched (pid %lu)", pi.dwProcessId);
    return 0;
}
#endif

}

int main(int argc, char** argv) {
    const bool perMonitorDpi = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
    if (!perMonitorDpi) SetProcessDPIAware();

    loadSettings(gSettings);
    loot::setFilter({gSettings.dzFilterMode, loot::parseList(gSettings.dzFilterList)});
    const bool autoStart = argc > 1;
    int monitor = 0;
    long wantW = 0, wantH = 0;
    if (autoStart) {
        gSettings.dma = false;
        gSettings.fullscreen = false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dma") gSettings.dma = true;
        else if (a == "--device" && i + 1 < argc) gSettings.device = argv[++i];
        else if (a == "--dma-dir" && i + 1 < argc) gSettings.dmaDir = argv[++i];
        else if (a == "--fullscreen") gSettings.fullscreen = true;
        else if (a == "--gdi") gForceGdi = true;
        else if (a == "--loot") gSettings.dzLoot = true;
        else if (a == "--radar") gSettings.dzRadar = true;
        else if (a == "--map" && i + 1 < argc) { gSettings.dzMap = argv[++i]; gSettings.dzMapAuto = false; }
        else if (a == "--local") gSettings.dma = false;
        else if (a == "--monitor" && i + 1 < argc) {
            const char* v = argv[++i];
            if (sscanf_s(v, "%ldx%ld", &wantW, &wantH) != 2) monitor = std::atoi(v);
        } else {
            MessageBoxA(nullptr,
                "Ickza [--local | --dma] [--device fpga] [--dma-dir dir] [--fullscreen]\n"
                "      [--monitor n|WxH] [--gdi] [--loot] [--radar] [--map name]\n"
                "no args = just the panel",
                "Ickza", a == "-h" || a == "--help" ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONERROR);
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    if (autoStart && gSettings.fullscreen && !gSettings.dma) {
        MessageBoxA(nullptr, "--fullscreen only applies with --dma", "Ickza", MB_OK | MB_ICONERROR);
        return 2;
    }
    if (monitor || wantW) {
        const auto displays = listDisplays();
        const DisplayInfo* pick = nullptr;
        for (const auto& d : displays)
            if (!pick && (wantW ? d.width() == wantW && d.height() == wantH : d.number == monitor)) pick = &d;
        if (!pick) {
            char msg[96];
            if (wantW) std::snprintf(msg, sizeof msg, "No display is %ldx%ld.", wantW, wantH);
            else std::snprintf(msg, sizeof msg, "No display %d.", monitor);
            MessageBoxA(nullptr, msg, "Ickza", MB_OK | MB_ICONERROR);
            return 2;
        }
        gSettings.displayDevice = pick->device;
        gSettings.displayW = pick->width();
        gSettings.displayH = pick->height();
    }

#ifndef ICKZA_NO_DIAG
    openLog();
    installCrashHandlers();
    DeleteFileA((exeDir0() + "\\Ickza_old.exe").c_str());
#endif
    const bool fineTimer = timeBeginPeriod(1) == TIMERR_NOERROR;
    std::string args;
    for (int i = 1; i < argc; ++i) args += std::string(" ") + argv[i];
    logLine("[log] Ickza " ICKZA_VERSION " started:%s (per-monitor dpi %s)", autoStart ? args.c_str() : " (panel)", perMonitorDpi ? "on" : "unavailable");

    tel::start([](const char* l) { logLine("%s", l); });

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
    ui::init();

    WNDCLASSA cc{};
    cc.lpfnWndProc = ctlProc;
    cc.hInstance = GetModuleHandleA(nullptr);
    cc.lpszClassName = "ickza_ctl";
    RegisterClassA(&cc);
    gCtl = CreateWindowExA(0, cc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cc.hInstance, nullptr);

    gPanel = createPanel(gSettings, {[] { startEsp(); }, [] { stopEsp(); }, [] { return gRun != Run::Idle; }, [] { return espInfo(); }});
    if (!gPanel) {
        logLine("[-] could not create the panel (error %lu)", GetLastError());
        return 1;
    }
    if (autoStart) {
        if (gSettings.dma) gSettings.dzRadarScreen = true;
        startEsp();
    }

#ifndef ICKZA_NO_DIAG
    bool updating = false;
#endif
    MSG m;
    for (bool quit = false; !quit;) {
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (quit) break;
#ifndef ICKZA_NO_DIAG
        if (updatePending()) {
            updating = true;
            break;
        }
#endif
        flushSettings();
        if (gRun == Run::Running && gOverlay) {
            runFrame();
            if (!gGpu && !gSettings.uncapped && FAILED(DwmFlush())) Sleep(5);
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        }
    }
    stopEsp();
    flushSettings(true);
    if (fineTimer) timeEndPeriod(1);
    tel::stop();
    ui::shutdown();
    Gdiplus::GdiplusShutdown(token);
    int exitCode = 0;
#ifndef ICKZA_NO_DIAG
    if (updating) exitCode = swapAndRelaunch();
    logLine("[log] quit%s", updating ? " (for update)" : "");
    if (gLog) std::fclose(gLog);
#endif
    TerminateProcess(GetCurrentProcess(), UINT(exitCode));
    return exitCode;
}
