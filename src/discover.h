#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "findings.h"
#include "image.h"
#include "process.h"
#include "profile.h"
#include "rtti.h"

struct Entity {
    uint64_t addr = 0;
    std::string cls;
    bool local = false;
};

struct Discovery {
    std::vector<VTableInfo> vtables;
    std::vector<Entity> entities;
    std::vector<Finding> findings;
    std::vector<std::string> log;
};

bool discoverEntities(const Process& p, const Image& img, const std::string& cls, Discovery& out, std::string& status,
                      const DiscoverConfig& cfg = {});

void findLootList(const Process& p, const Image& img, const DiscoverConfig& cfg, int waitSec,
                  const std::function<void(const std::string&)>& log, const std::function<void(int)>& wait);

void findItemNames(const Process& p, const Image& img, const DiscoverConfig& cfg, uint32_t listOff, int n,
                   const std::function<void(const std::string&)>& log);

void findPlayerNames(const Process& p, const Image& img, const DiscoverConfig& cfg, int n,
                     const std::function<void(const std::string&)>& log);

void findMyName(const Process& p, const Image& img, const DiscoverConfig& cfg, const std::string& name,
               const std::function<void(const std::string&)>& log);

void findScoreboard(const Process& p, const Image& img, const DiscoverConfig& cfg, const std::string& myName,
                    const std::function<void(const std::string&)>& log);

void findNameChain(const Process& p, const Image& img, const DiscoverConfig& cfg,
                   const std::function<void(const std::string&)>& log);

void findClassNames(const Process& p, const Image& img, const std::string& className, const std::string& myName,
                    const std::function<void(const std::string&)>& log);

struct ClassInfo {
    std::string name;
    std::vector<std::string> bases;
};
std::vector<ClassInfo> listClasses(const Image& img, const std::string& filter);

std::string classNameAt(const Process& p, const Image& img, uint64_t addr);

std::vector<uint32_t> findCodeXrefs(const Image& img, uint32_t rva, size_t maxHits = 1024);

bool findRenderMatrices(const Image& img, std::vector<Finding>& out, std::string& status);

void findHandItem(const Process& p, const Image& img, const DiscoverConfig& cfg, int waitSec,
                  const std::function<void(const std::string&)>& log, const std::function<void(int)>& wait);

void findNetRoster(const Process& p, const Image& img, const DiscoverConfig& cfg,
                   const std::function<void(const std::string&)>& log);

void findWorldGlobal(const Process& p, const Image& img, const DiscoverConfig& cfg,
                     const std::vector<Entity>& entities, const std::function<void(const std::string&)>& log);
