#pragma once

#include <string>
#include <vector>

namespace mapdl {

struct Entry {
    const char* id;
    const char* xam;
    const char* version;
    const char* label;
    int size;
};

const std::vector<Entry>& catalog();
const Entry* find(const std::string& id);

bool official(const std::string& id);

std::string label(const std::string& id);

void setDir(const std::string& mapsDir);
#ifndef ICKZA_NO_DIAG
void setLog(void (*fn)(const char*));
#endif

float savedSize(const std::string& id);

bool start(const std::string& id, bool topo, bool high);
void cancel();

struct Status {
    bool busy = false;
    std::string id;
    bool topo = false;
    std::string phase;
    int done = 0, total = 0;
    double mbps = 0;
    std::string error;
    int generation = 0;
};
Status status();

}
