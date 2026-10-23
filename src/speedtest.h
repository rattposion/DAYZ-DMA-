#pragma once
#include <atomic>

class Process;

namespace speed {

double quick(const Process& p, int iters = 4);

inline std::atomic<double> gLinkMBps{0};

}
