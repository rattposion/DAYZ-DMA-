
#define NOMINMAX
#include "mapdl.h"

#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace mapdl {
namespace {

const std::vector<Entry> kCatalog = {
    {"chernarusplus", "chernarusplus", "1.30", "Chernarus", 15360},
    {"livonia", "livonia", "1.30", "Livonia", 12800},
    {"sakhal", "sakhal", "1.30", "Sakhal (Frostline)", 15360},
    {"nasdara", "nasdara", "1.30-9", "Nasdara (Badlands)", 16384},
    {"alteria", "alteria", "May.3-5", "Alteria", 8192},
    {"anastara", "anastara", "Jan.15-11", "Anastara", 10240},
    {"antoria", "antoria", "Jul.12-2", "Antoria", 20480},
    {"arsteinen", "arsteinen", "Dec.13-11", "Arsteinen", 15360},
    {"avalon", "avalon", "Apr.9-4", "Avalon", 20480},
    {"azalea", "azalea", "0.3.4-5", "Azalea", 20480},
    {"banov", "banov", "Jun.15", "Banov", 15360},
    {"bitterroot", "bitterroot", "Jan.11-11", "Bitterroot", 12288},
    {"channel", "channel", "Apr.30-4", "The Channel", 20480},
    {"chernarus2035", "chernarus2035", "Mar.29", "Chernarus 2035", 20480},
    {"chiemsee", "chiemsee", "4.0-11", "Chiemsee", 10240},
    {"deadfall", "deadfall", "Jul.6-9", "Deadfall", 10240},
    {"deerisle", "deerisle", "5.923-12", "Deer Isle", 16384},
    {"esseker", "esseker", "0.58-26", "Esseker", 12800},
    {"hashima", "hashima", "Jun.8-6", "Hashima Islands", 5120},
    {"iztek", "iztek", "2.0.4-14", "Iztek", 8192},
    {"lux", "lux", "May.29-3", "Lux", 15360},
    {"malvinas", "malvinas", "Sep.16-5", "Malvinas", 12288},
    {"melkart", "melkart", "Dec.30-4", "Melkart", 20480},
    {"namalsk", "namalsk", "Apr.8-4", "Namalsk", 12800},
    {"nhchernobyl", "nhchernobyl", "Jul.22-11", "NH Chernobyl", 20480},
    {"novikostok", "novikostok", "Oct.29-5", "Novikostok", 15360},
    {"nukezzone", "nukezzone", "Jul.24-11", "NukeZZone", 15360},
    {"nyheim", "nyheim", "Apr.25-4", "Nyheim", 15360},
    {"onforin", "onforin", "Apr.8-11", "Onforin", 24576},
    {"pnw", "pnw", "2.0-13", "PNW", 10240},
    {"pripyat", "pripyat", "0.94a-11", "Pripyat", 20480},
    {"raman", "raman", "Mar.14-11", "Raman", 32768},
    {"ros", "ros", "Jul.28-11", "ROS", 20480},
    {"rostow", "rostow", "1.122-11", "Rostow", 14336},
    {"sahrani", "sahrani", "Jan.23-5", "Sahrani", 20480},
    {"siberia", "siberia", "May.13", "Siberia", 14047},
    {"stuartisland", "stuartisland", "Apr.27-11", "Stuart Island", 5120},
    {"takistan", "takistanplus", "1.0631-11", "Takistan Plus", 12800},
    {"valning", "valning", "Nov.26-14", "Valning", 10240},
    {"vela", "vela", "Dec.29-11", "Vela", 10240},
    {"visisland", "visisland", "0.04-11", "Vis Island", 20480},
    {"yiprit", "yiprit", "Jan.29-14", "Yiprit", 20480},
};

constexpr wchar_t kSiteHost[] = L"dayz.xam.nu";
constexpr wchar_t kDataHost[] = L"static.xam.nu";
constexpr int kWorkers = 12;
constexpr int kRowsAhead = 6;
constexpr int kTile = 256;
constexpr uint8_t kMissB = 30, kMissG = 24, kMissR = 20;

std::string gDir;
#ifndef ICKZA_NO_DIAG
void (*gLog)(const char*) = nullptr;
void dlog(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (gLog) gLog(b);
    else OutputDebugStringA(b);
}
#endif

std::mutex gMx;
Status gSt;
std::map<std::string, float> gSizes;
std::atomic<bool> gCancel{false};
std::atomic<int> gDone{0};
std::atomic<long long> gBytes{0};
ULONGLONG gT0 = 0;

void setPhase(const char* p) {
    std::lock_guard<std::mutex> lk(gMx);
    gSt.phase = p;
}

bool fileExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring widen(const std::string& s) {
    std::wstring w(s.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    w.resize(n > 0 ? size_t(n - 1) : 0);
    return w;
}

HINTERNET session() {
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Ickza", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h)
            h = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Ickza", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) WinHttpSetTimeouts(h, 10000, 10000, 15000, 30000);
        return h;
    }();
    return s;
}

struct Conn {
    HINTERNET h = nullptr;
    explicit Conn(const wchar_t* host) {
        if (HINTERNET s = session()) h = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    ~Conn() {
        if (h) WinHttpCloseHandle(h);
    }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    int get(const std::string& path, std::string& body) {
        body.clear();
        if (!h) return -1;
        HINTERNET r = WinHttpOpenRequest(h, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
        if (!r) return -1;
        int status = -1;
        const wchar_t* hdr = L"Referer: https://dayz.xam.nu/\r\n";
        if (WinHttpSendRequest(r, hdr, DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, len = sizeof code;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                                    WINHTTP_NO_HEADER_INDEX))
                status = int(code);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) {
                    status = -1;
                    break;
                }
                if (!avail) break;
                const size_t at = body.size();
                body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, body.data() + at, avail, &got)) {
                    status = -1;
                    break;
                }
                body.resize(at + got);
                gBytes += got;
                if (gCancel) break;
            }
        }
        WinHttpCloseHandle(r);
        return status;
    }
};

std::string jsonStr(const std::string& j, const char* key, size_t from = 0, size_t to = std::string::npos) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t a = j.find(k, from);
    if (a == std::string::npos || a >= to) return "";
    const size_t s = a + k.size(), e = j.find('"', s);
    return e == std::string::npos ? "" : j.substr(s, e - s);
}
double jsonNum(const std::string& j, const char* key, size_t from = 0, size_t to = std::string::npos) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t a = j.find(k, from);
    if (a == std::string::npos || a >= to) return 0;
    return std::strtod(j.c_str() + a + k.size(), nullptr);
}
size_t matchClose(const std::string& j, size_t at) {
    int depth = 0;
    bool str = false;
    for (size_t i = at; i < j.size(); ++i) {
        const char c = j[i];
        if (str) {
            if (c == '\\') ++i;
            else if (c == '"') str = false;
        } else if (c == '"') {
            str = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}
std::string readJsonString(const std::string& j, size_t s) {
    std::string out;
    for (size_t i = s; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] != '\\') {
            out += j[i];
            continue;
        }
        if (++i >= j.size()) break;
        if (j[i] == 'u') {
            out += '?';
            i += 4;
        } else {
            out += j[i] == 'n' || j[i] == 't' ? ' ' : j[i];
        }
    }
    return out;
}

std::map<std::string, std::string> liveVersions() {
    static std::mutex mx;
    static bool tried = false;
    static std::map<std::string, std::string> out;
    std::lock_guard<std::mutex> lk(mx);
    if (tried) return out;
    tried = true;
    Conn site(kSiteHost);
    std::string html, js;
    if (site.get("/", html) != 200) return out;
    const size_t a = html.find("/js/bundle.");
    if (a == std::string::npos) return out;
    const size_t e = html.find(".js", a);
    if (e == std::string::npos) return out;
    if (site.get(html.substr(a, e + 3 - a), js) != 200) return out;
    const size_t t = js.find("{\"chernarusplus\":{\"v\":\"");
    if (t == std::string::npos) return out;
    const size_t end = matchClose(js, t);
    if (end == std::string::npos) return out;
    const std::string tbl = js.substr(t, end + 1 - t);
    for (size_t p = 1; (p = tbl.find("\":{\"v\":\"", p)) != std::string::npos; ++p) {
        const size_t ns = tbl.rfind('"', p - 1);
        if (ns == std::string::npos) continue;
        const std::string name = tbl.substr(ns + 1, p - ns - 1);
        const size_t vs = p + 8, ve = tbl.find('"', vs);
        if (ve == std::string::npos) break;
        const std::string v = tbl.substr(vs, ve - vs);
        const size_t close = tbl.find('}', ve);
        const int it = int(jsonNum(tbl, "i", ve, close));
        out[name] = it ? v + "-" + std::to_string(it) : v;
    }
    dlog("[*] maps: XAM site lists %zu maps", out.size());
    return out;
}

std::string titleCase(std::string s) {
    bool up = true;
    for (char& c : s) {
        if (c == '_') c = ' ';
        const unsigned char u = static_cast<unsigned char>(c);
        if (up && std::isalpha(u)) c = char(std::toupper(u));
        up = c == ' ' || c == '-' || c == '(' || c == '/';
    }
    return s;
}

int writeNames(const std::string& j, float size, const std::string& path) {
    const size_t a = j.find("\"locations\":[");
    if (a == std::string::npos) return 0;
    const size_t arr = a + 12, end = matchClose(j, arr);
    if (end == std::string::npos) return 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") || !f) return 0;
    int n = 0;
    for (size_t o = j.find('{', arr); o != std::string::npos && o < end;) {
        const size_t oe = matchClose(j, o);
        if (oe == std::string::npos || oe > end) break;
        std::string w = jsonStr(j, "w", o, oe);
        const size_t pa = j.find("\"p\":[", o);
        const size_t sa = j.find("\"s\":[\"", o);
        if (!w.empty() && pa < oe && sa < oe) {
            char* q = nullptr;
            const double lat = std::strtod(j.c_str() + pa + 5, &q);
            const double lng = (q && *q == ',') ? std::strtod(q + 1, nullptr) : 0;
            const std::string name = titleCase(readJsonString(j, sa + 6));
            const float x = float(lng / 256.0 * size), z = float((256.0 + lat) / 256.0 * size);
            if (!name.empty() && x >= 0 && z >= 0 && x <= size && z <= size) {
                if (w == "namemarine") w = "marine";
                w[0] = char(std::toupper(static_cast<unsigned char>(w[0])));
                std::fprintf(f, "%.1f %.1f %s %s\n", x, z, w.c_str(), name.c_str());
                ++n;
            }
        }
        o = j.find('{', oe);
    }
    std::fclose(f);
    return n;
}

struct Band {
    std::vector<uint8_t> px;
    int left = 0;
};

void fail(const std::string& why) {
    dlog("[-] maps: %s", why.c_str());
    std::lock_guard<std::mutex> lk(gMx);
    gSt.error = why;
}

void finish() {
    std::lock_guard<std::mutex> lk(gMx);
    gSt.busy = false;
    gSt.phase.clear();
}

void job(std::string id, bool topo, bool high) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoGuard {
        HRESULT hr;
        ~CoGuard() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } coGuard{co};
    CreateDirectoryA(gDir.c_str(), nullptr);

    const Entry* e = find(id);
    const std::string xam = e ? e->xam : id;
    std::string version = e ? e->version : "";
    {
        const auto live = liveVersions();
        const auto it = live.find(xam);
        if (it != live.end()) version = it->second;
    }
    if (version.empty()) {
        fail(label(id) + " is not on the XAM map site");
        return finish();
    }

    Conn data(kDataHost);
    std::string j;
    int code = data.get("/dayz/json/" + xam + "/" + version + ".json", j);
    if (code != 200 && e && version != e->version) code = data.get("/dayz/json/" + xam + "/" + e->version + ".json", j);
    if (gCancel) return finish();
    if (code != 200) {
        fail(code < 0 ? "no connection to static.xam.nu - is the internet up?" : "map data not found (HTTP " + std::to_string(code) + ")");
        return finish();
    }
    const size_t ia = j.find("\"info\":{");
    const size_t ie = ia == std::string::npos ? ia : j.find('}', ia);
    const std::string tiles = jsonStr(j, "tiles", ia, ie), tilev = jsonStr(j, "tilev", ia, ie);
    std::string fmt = jsonStr(j, "format", ia, ie);
    if (fmt.empty()) fmt = "jpg";
    const float size = float(jsonNum(j, "size", ia, ie));
    if (ia == std::string::npos || tiles.empty() || tilev.empty() || size <= 0) {
        fail("map data for " + label(id) + " has no tile info");
        return finish();
    }

    const std::string names = gDir + id + "_names.txt";
    if (!fileExists(names)) {
        const int n = writeNames(j, size, names);
        dlog("[*] maps: %d place names for %s", n, id.c_str());
        if (!n) DeleteFileA(names.c_str());
    }
    if (FILE* f = nullptr; !fopen_s(&f, (gDir + id + ".size").c_str(), "w") && f) {
        std::fprintf(f, "%g\n", size);
        std::fclose(f);
    }
    {
        std::lock_guard<std::mutex> lk(gMx);
        gSizes.erase(id);
    }
    j.clear();
    j.shrink_to_fit();

    const int zoom = high ? 6 : 5, N = 1 << zoom, W = kTile * N, total = N * N;
    const size_t stride = size_t(W) * 3;
    {
        std::lock_guard<std::mutex> lk(gMx);
        gSt.phase = "Downloading";
        gSt.total = total;
    }
    dlog("[*] maps: %s %s %dx%d px from %s/%s (%s)", id.c_str(), topo ? "topographic" : "satellite", W, W, tiles.c_str(), tilev.c_str(),
         fmt.c_str());

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        fail("Windows imaging (WIC) unavailable");
        return finish();
    }
    const std::string base = gDir + id + (topo ? "_topo" : "");
    const std::string part = base + ".jpg.part", out = base + ".jpg";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    bool okEnc = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(widen(part).c_str(), GENERIC_WRITE)) &&
                 SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
                 SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, &props));
    if (okEnc) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.9f;
        props->Write(1, &opt, &v);
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        okEnc = SUCCEEDED(frame->Initialize(props.Get())) && SUCCEEDED(frame->SetSize(UINT(W), UINT(W))) &&
                SUCCEEDED(frame->SetPixelFormat(&pf)) && IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR);
    }
    if (!okEnc) {
        frame.Reset();
        enc.Reset();
        stream.Reset();
        DeleteFileA(part.c_str());
        fail("cannot write " + out);
        return finish();
    }

    std::mutex mx;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Band>> bands(static_cast<size_t>(N));
    int written = 0;
    std::atomic<int> next{0};
    std::atomic<int> missing{0};
    std::atomic<bool> fatal{false};
    std::string fatalWhy;

    auto band = [&](int y) -> Band& {
        auto& b = bands[size_t(y)];
        if (!b) {
            b = std::make_unique<Band>();
            b->left = N;
            b->px.resize(stride * kTile);
            for (size_t i = 0; i < b->px.size(); i += 3) {
                b->px[i] = kMissB;
                b->px[i + 1] = kMissG;
                b->px[i + 2] = kMissR;
            }
        }
        return *b;
    };
    auto stop = [&](const std::string& why) {
        {
            std::lock_guard<std::mutex> lk(mx);
            if (!fatal) fatalWhy = why;
            fatal = true;
        }
        cv.notify_all();
    };

    auto worker = [&] {
        const HRESULT wco = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IWICImagingFactory> f;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
            Conn c(kDataHost);
            std::string body;
            const std::string pre = "/dayz/maps/" + tiles + "/" + tilev + "/" + (topo ? "topographic/" : "satellite/") + std::to_string(zoom) + "/";
            for (;;) {
                const int t = next++;
                if (t >= total || gCancel || fatal) break;
                const int x = t % N, y = t / N;
                {
                    std::unique_lock<std::mutex> lk(mx);
                    cv.wait(lk, [&] { return y < written + kRowsAhead || gCancel || fatal; });
                    if (gCancel || fatal) break;
                    band(y);
                }
                int st = -1;
                for (int attempt = 0; attempt < 4 && !gCancel; ++attempt) {
                    st = c.get(pre + std::to_string(x) + "/" + std::to_string(y) + "." + fmt, body);
                    if (st == 200 || st == 404 || st == 403) break;
                    Sleep(300 * (attempt + 1));
                }
                if (gCancel) break;
                bool landed = st == 404 || st == 403;
                if (st == 404 || st == 403) ++missing;
                if (st == 200 && f) {
                    ComPtr<IWICStream> ms;
                    ComPtr<IWICBitmapDecoder> dec;
                    ComPtr<IWICBitmapFrameDecode> fr;
                    ComPtr<IWICFormatConverter> conv;
                    ComPtr<IWICBitmapScaler> sc;
                    UINT tw = 0, th = 0;
                    if (SUCCEEDED(f->CreateStream(&ms)) && SUCCEEDED(ms->InitializeFromMemory(reinterpret_cast<BYTE*>(body.data()), DWORD(body.size()))) &&
                        SUCCEEDED(f->CreateDecoderFromStream(ms.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
                        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&tw, &th))) {
                        IWICBitmapSource* src = fr.Get();
                        if ((tw != kTile || th != kTile) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
                            SUCCEEDED(sc->Initialize(fr.Get(), kTile, kTile, WICBitmapInterpolationModeFant)))
                            src = sc.Get();
                        if (SUCCEEDED(f->CreateFormatConverter(&conv)) &&
                            SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                            uint8_t* dst;
                            {
                                std::lock_guard<std::mutex> lk(mx);
                                dst = band(y).px.data() + size_t(x) * kTile * 3;
                            }
                            const WICRect rc{0, 0, kTile, kTile};
                            landed = SUCCEEDED(conv->CopyPixels(&rc, UINT(stride), UINT(stride * (kTile - 1) + kTile * 3), dst));
                        }
                    }
                    if (!landed) {
                        stop(fmt == "webp" ? "Windows can't decode WebP - install \"WebP Image Extensions\" from the Microsoft Store"
                                           : "a map tile could not be decoded");
                        break;
                    }
                }
                if (!landed) {
                    stop(st < 0 ? "lost the connection to static.xam.nu" : "the map site refused a tile (HTTP " + std::to_string(st) + ")");
                    break;
                }
                ++gDone;
                bool rowDone;
                {
                    std::lock_guard<std::mutex> lk(mx);
                    rowDone = --band(y).left == 0;
                }
                if (rowDone) cv.notify_all();
            }
        }
        if (SUCCEEDED(wco)) CoUninitialize();
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < kWorkers; ++i) pool.emplace_back(worker);

    bool okRows = true;
    for (int y = 0; y < N; ++y) {
        std::unique_ptr<Band> b;
        {
            std::unique_lock<std::mutex> lk(mx);
            cv.wait(lk, [&] { return (bands[size_t(y)] && bands[size_t(y)]->left == 0) || gCancel || fatal; });
            if (gCancel || fatal) {
                okRows = false;
                break;
            }
            b = std::move(bands[size_t(y)]);
        }
        if (FAILED(frame->WritePixels(kTile, UINT(stride), UINT(b->px.size()), b->px.data()))) {
            stop("writing " + out + " failed (disk full?)");
            okRows = false;
            break;
        }
        {
            std::lock_guard<std::mutex> lk(mx);
            written = y + 1;
        }
        cv.notify_all();
    }
    cv.notify_all();
    for (auto& t : pool) t.join();

    if (okRows && missing > total / 2) {
        okRows = false;
        fatalWhy = "the site has no " + std::string(topo ? "topographic" : "satellite") + " tiles for " + label(id);
    }
    if (okRows) {
        setPhase("Saving");
        okRows = SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        if (!okRows) fatalWhy = "saving " + out + " failed";
    }
    frame.Reset();
    enc.Reset();
    props.Reset();
    stream.Reset();
    if (okRows) {
        okRows = false;
        for (int i = 0; i < 20 && !okRows; ++i) {
            okRows = MoveFileExA(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
            if (!okRows) Sleep(250);
        }
        if (!okRows) fatalWhy = "could not replace " + out + " (in use?)";
    }
    if (!okRows) DeleteFileA(part.c_str());

    const double secs = double(GetTickCount64() - gT0) / 1000.0;
    if (okRows) {
        dlog("[+] maps: %s ready (%d tiles, %d empty, %.1f MB in %.0f s)", out.c_str(), total, missing.load(), double(gBytes) / 1048576.0, secs);
        std::lock_guard<std::mutex> lk(gMx);
        ++gSt.generation;
    } else if (gCancel) {
        dlog("[*] maps: %s download cancelled", id.c_str());
    } else {
        fail(fatalWhy.empty() ? "download failed" : fatalWhy);
    }
    finish();
}

}

const std::vector<Entry>& catalog() { return kCatalog; }

bool official(const std::string& id) { return id == "chernarusplus" || id == "livonia" || id == "sakhal" || id == "nasdara"; }

const Entry* find(const std::string& id) {
    for (const Entry& e : kCatalog)
        if (id == e.id) return &e;
    return nullptr;
}

std::string label(const std::string& id) {
    const Entry* e = find(id);
    return e ? e->label : id;
}

void setDir(const std::string& mapsDir) {
    std::lock_guard<std::mutex> lk(gMx);
    gDir = mapsDir;
    gSizes.clear();
}
#ifndef ICKZA_NO_DIAG
void setLog(void (*fn)(const char*)) { gLog = fn; }
#endif

float savedSize(const std::string& id) {
    std::lock_guard<std::mutex> lk(gMx);
    if (gDir.empty()) return 0;
    const auto it = gSizes.find(id);
    if (it != gSizes.end()) return it->second;
    float v = 0;
    if (FILE* f = nullptr; !fopen_s(&f, (gDir + id + ".size").c_str(), "r") && f) {
        if (fscanf_s(f, "%f", &v) != 1 || v < 256 || v > 100000) v = 0;
        std::fclose(f);
    }
    gSizes[id] = v;
    return v;
}

bool start(const std::string& id, bool topo, bool high) {
    if (id.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(gMx);
        if (gSt.busy) return false;
        gSt.busy = true;
        gSt.id = id;
        gSt.topo = topo;
        gSt.phase = "Looking up";
        gSt.done = gSt.total = 0;
        gSt.error.clear();
    }
    gCancel = false;
    gDone = 0;
    gBytes = 0;
    gT0 = GetTickCount64();
    dlog("[*] maps: downloading %s (%s, %s)", id.c_str(), topo ? "topographic" : "satellite", high ? "16384 px" : "8192 px");
    try {
        std::thread(job, id, topo, high).detach();
    } catch (...) {
        fail("could not start the download thread");
        finish();
        return false;
    }
    return true;
}

void cancel() { gCancel = true; }

Status status() {
    std::lock_guard<std::mutex> lk(gMx);
    Status s = gSt;
    if (s.busy) {
        s.done = gDone;
        const double secs = double(GetTickCount64() - gT0) / 1000.0;
        s.mbps = secs > 0.5 ? double(gBytes) / 1048576.0 / secs : 0;
    }
    return s;
}

}
