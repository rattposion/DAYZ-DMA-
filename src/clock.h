#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

inline uint64_t qpcUs() {
    static const uint64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return uint64_t(x.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t q = uint64_t(c.QuadPart);
    return q / f * 1000000ull + q % f * 1000000ull / f;
}
