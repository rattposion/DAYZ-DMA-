#include "image.h"

#include <algorithm>
#ifndef ICKZA_NO_DIAG
#include <fstream>
#include <iterator>
#endif

static void readTolerant(const Process& p, uintptr_t addr, uint8_t* dst, size_t n) {
    if (p.read(addr, dst, n)) return;
    for (size_t off = 0; off < n; off += 0x1000) {
        size_t chunk = std::min<size_t>(0x1000, n - off);
        p.read(addr + off, dst + off, chunk);
    }
}

bool Image::load(const Process& p, std::string& err) {
    const ModuleInfo& m = p.module();
    base = m.base;

    std::vector<uint8_t> hdr(0x1000);
    if (!p.read(base, hdr.data(), hdr.size())) {
        err = "cannot read PE header";
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hdr.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > hdr.size()) {
        err = "bad DOS header";
        return false;
    }
    auto* nt32 = reinterpret_cast<IMAGE_NT_HEADERS32*>(hdr.data() + dos->e_lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE) {
        err = "bad NT signature";
        return false;
    }

    const WORD nsec = nt32->FileHeader.NumberOfSections;
    timestamp = nt32->FileHeader.TimeDateStamp;
    const IMAGE_SECTION_HEADER* sec = nullptr;
    uint32_t sizeOfImage = 0;
    if (nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        auto* nt64 = reinterpret_cast<IMAGE_NT_HEADERS64*>(nt32);
        ptrSize = 8;
        preferredBase = nt64->OptionalHeader.ImageBase;
        sizeOfImage = nt64->OptionalHeader.SizeOfImage;
        sec = IMAGE_FIRST_SECTION(nt64);
    } else {
        ptrSize = 4;
        preferredBase = nt32->OptionalHeader.ImageBase;
        sizeOfImage = nt32->OptionalHeader.SizeOfImage;
        sec = IMAGE_FIRST_SECTION(nt32);
    }
    if (reinterpret_cast<const uint8_t*>(sec + nsec) > hdr.data() + hdr.size()) {
        err = "section table runs past header page";
        return false;
    }

    bytes.assign(std::max<size_t>(sizeOfImage, m.size), 0);
    std::memcpy(bytes.data(), hdr.data(), std::min(hdr.size(), bytes.size()));

    for (WORD i = 0; i < nsec; ++i) {
        Section s;
        s.name.assign(reinterpret_cast<const char*>(sec[i].Name), strnlen(reinterpret_cast<const char*>(sec[i].Name), 8));
        s.rva = sec[i].VirtualAddress;
        s.vsize = sec[i].Misc.VirtualSize;
        s.flags = sec[i].Characteristics;
        sections.push_back(s);
        if (s.rva >= bytes.size()) continue;
        size_t n = std::min<size_t>(s.vsize, bytes.size() - s.rva);
        readTolerant(p, base + s.rva, &bytes[s.rva], n);
    }
    return true;
}

#ifndef ICKZA_NO_DIAG
bool Image::loadFile(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 0x400) {
        err = "file too small";
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size()) {
        err = "bad DOS header";
        return false;
    }
    auto* nt32 = reinterpret_cast<IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE) {
        err = "bad NT signature";
        return false;
    }
    const WORD nsec = nt32->FileHeader.NumberOfSections;
    timestamp = nt32->FileHeader.TimeDateStamp;
    const IMAGE_SECTION_HEADER* sec = nullptr;
    uint32_t sizeOfImage = 0, sizeOfHeaders = 0;
    if (nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        auto* nt64 = reinterpret_cast<IMAGE_NT_HEADERS64*>(nt32);
        ptrSize = 8;
        preferredBase = nt64->OptionalHeader.ImageBase;
        sizeOfImage = nt64->OptionalHeader.SizeOfImage;
        sizeOfHeaders = nt64->OptionalHeader.SizeOfHeaders;
        sec = IMAGE_FIRST_SECTION(nt64);
    } else {
        ptrSize = 4;
        preferredBase = nt32->OptionalHeader.ImageBase;
        sizeOfImage = nt32->OptionalHeader.SizeOfImage;
        sizeOfHeaders = nt32->OptionalHeader.SizeOfHeaders;
        sec = IMAGE_FIRST_SECTION(nt32);
    }
    if (reinterpret_cast<const uint8_t*>(sec + nsec) > file.data() + file.size()) {
        err = "section table runs past the file";
        return false;
    }
    base = static_cast<uintptr_t>(preferredBase);
    bytes.assign(sizeOfImage, 0);
    std::memcpy(bytes.data(), file.data(), std::min<size_t>({sizeOfHeaders, file.size(), bytes.size()}));
    for (WORD i = 0; i < nsec; ++i) {
        Section s;
        s.name.assign(reinterpret_cast<const char*>(sec[i].Name), strnlen(reinterpret_cast<const char*>(sec[i].Name), 8));
        s.rva = sec[i].VirtualAddress;
        s.vsize = sec[i].Misc.VirtualSize;
        s.flags = sec[i].Characteristics;
        sections.push_back(s);
        const size_t raw = sec[i].PointerToRawData;
        if (s.rva >= bytes.size() || raw >= file.size()) continue;
        const size_t n = std::min<size_t>({sec[i].SizeOfRawData, s.vsize ? s.vsize : sec[i].SizeOfRawData, bytes.size() - s.rva,
                                           file.size() - raw});
        std::memcpy(&bytes[s.rva], &file[raw], n);
    }
    return true;
}
#endif

const Section* Image::sectionFor(uint64_t rva) const {
    for (const auto& s : sections)
        if (rva >= s.rva && rva < static_cast<uint64_t>(s.rva) + s.vsize) return &s;
    return nullptr;
}
