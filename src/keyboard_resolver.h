#pragma once

#include "keyboard_bitmap.h"
#include <vmmdll.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dma_keyboard {

inline constexpr DWORD kKernel = VMMDLL_PID_PROCESS_WITH_KERNELMEMORY;
inline constexpr int kWildcard = -1;

struct Reader {
    VMM_HANDLE handle = nullptr;
    decltype(&VMMDLL_MemReadEx) read = nullptr;
    decltype(&VMMDLL_ProcessGetModuleBaseU) module = nullptr;
    decltype(&VMMDLL_ProcessGetInformationAll) processes = nullptr;
    decltype(&VMMDLL_MemFree) free = nullptr;
    decltype(&VMMDLL_WinReg_QueryValueExU) registry = nullptr;

    bool exact(DWORD pid, uint64_t address, void* out, DWORD size) const {
        DWORD got = 0;
        return read(handle, pid, address, static_cast<PBYTE>(out), size, &got,
                    VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT) && got == size;
    }

    template<class T>
    bool exact(DWORD pid, uint64_t address, T& out) const {
        return exact(pid, address, &out, sizeof out);
    }

    std::pair<DWORD, DWORD> version() const {
        constexpr char root[] = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\";
        const std::string buildPath = std::string(root) + "CurrentBuild";
        const std::string revisionPath = std::string(root) + "UBR";
        wchar_t build[32]{};
        DWORD revision = 0, type = 0, size = sizeof build - sizeof(wchar_t);
        if (!registry(handle, buildPath.c_str(), &type, reinterpret_cast<PBYTE>(build), &size) || type != REG_SZ)
            throw std::runtime_error("Could not read target Windows build.");
        size = sizeof revision;
        if (!registry(handle, revisionPath.c_str(), &type, reinterpret_cast<PBYTE>(&revision), &size) ||
            type != REG_DWORD || size != sizeof revision)
            throw std::runtime_error("Could not read target Windows revision.");
        return {std::stoul(build), revision};
    }
};

struct Pattern {
    const char* name;
    std::vector<int> bytes;
    size_t operand;
    bool ripRelative;
    std::vector<uint64_t> targets;
};

template<class T>
T field(const uint8_t* bytes) {
    T value{};
    std::memcpy(&value, bytes, sizeof value);
    return value;
}

inline unsigned scan(const Reader& vmm, DWORD pid, DWORD alternate, const char* module, std::vector<Pattern>& patterns) {
    const uint64_t base = vmm.module(vmm.handle, pid, module);
    uint8_t header[4096]{};
    if (!base || !vmm.exact(pid, base, header)) throw std::runtime_error("Module PE header unavailable.");
    const auto dos = field<IMAGE_DOS_HEADER>(header);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof dos ||
        size_t(dos.e_lfanew) > sizeof header - sizeof(IMAGE_NT_HEADERS64))
        throw std::runtime_error("Invalid DOS header.");
    const auto nt = field<IMAGE_NT_HEADERS64>(header + dos.e_lfanew);
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
        throw std::runtime_error("Invalid PE32+ header.");
    const size_t table = size_t(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                         nt.FileHeader.SizeOfOptionalHeader;
    if (table > sizeof header || nt.FileHeader.NumberOfSections > (sizeof header - table) / sizeof(IMAGE_SECTION_HEADER))
        throw std::runtime_error("Invalid section table.");
    bool scanned = false;
    unsigned missingPages = 0;
    const bool sameModule = alternate && vmm.module(vmm.handle, alternate, module) == base;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const auto section = field<IMAGE_SECTION_HEADER>(header + table + i * sizeof(IMAGE_SECTION_HEADER));
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !section.Misc.VirtualSize) continue;
        const DWORD size = section.Misc.VirtualSize;
        if (size > 16u * 1024 * 1024 || uint64_t(section.VirtualAddress) + size > nt.OptionalHeader.SizeOfImage)
            throw std::runtime_error("Invalid executable section.");
        const uint64_t code = base + section.VirtualAddress;
        std::vector<uint8_t> bytes(size);
        constexpr DWORD pageSize = 4096;
        std::vector<bool> readable((size + pageSize - 1) / pageSize, true);
        if (!vmm.exact(pid, code, bytes.data(), size)) {
            for (DWORD offset = 0; offset < size; offset += pageSize) {
                const DWORD length = std::min(pageSize, size - offset);
                const bool ok = vmm.exact(pid, code + offset, bytes.data() + offset, length) ||
                    (sameModule && vmm.exact(alternate, code + offset, bytes.data() + offset, length));
                readable[offset / pageSize] = ok;
                if (!ok) ++missingPages;
            }
        }
        scanned = true;
        for (auto& pattern : patterns) {
            for (size_t at = 0; at + pattern.bytes.size() <= bytes.size(); ++at) {
                if (!readable[at / pageSize] || !readable[(at + pattern.bytes.size() - 1) / pageSize]) continue;
                size_t j = 0;
                for (; j < pattern.bytes.size(); ++j) {
                    const int b = pattern.bytes[j];
                    if (b != kWildcard && b != bytes[at + j]) break;
                }
                if (j != pattern.bytes.size()) continue;
                const auto* operand = bytes.data() + at + pattern.operand;
                const auto target = pattern.ripRelative ? code + at + 7 + field<int32_t>(operand)
                                                        : uint64_t(field<uint32_t>(operand));
                pattern.targets.push_back(target);
            }
        }
    }
    if (!scanned) throw std::runtime_error("No executable section found.");
    return missingPages;
}

inline bool kernelAddress(uint64_t address) { return address >= 0xffff800000000000ull; }

inline std::pair<DWORD, DWORD> proxies(const Reader& vmm, DWORD session) {
    PVMMDLL_PROCESS_INFORMATION all = nullptr;
    DWORD n = 0;
    if (!vmm.processes(vmm.handle, &all, &n) || !all)
        throw std::runtime_error("Could not enumerate target processes.");
    DWORD csrss = 0, winlogon = 0;
    unsigned csrssCount = 0, winlogonCount = 0;
    for (DWORD i = 0; i < n; ++i) {
        const auto& p = all[i];
        if (p.dwState || p.win.dwSessionId != session) continue;
        if (_stricmp(p.szName, "csrss.exe") == 0) {
            ++csrssCount;
            csrss = p.dwPID;
        }
        if (_stricmp(p.szName, "winlogon.exe") == 0) {
            ++winlogonCount;
            winlogon = p.dwPID;
        }
    }
    vmm.free(all);
    if (csrssCount != 1 || winlogonCount != 1)
        throw std::runtime_error("Expected one live csrss and winlogon in the requested session.");
    return {csrss, winlogon};
}

struct Address {
    DWORD pid = 0;
    uint64_t va = 0;
};

inline void printMatches(const char* module, const std::vector<Pattern>& patterns) {
    for (const auto& pattern : patterns) {
        std::printf("%s %s: %zu matches", module, pattern.name, pattern.targets.size());
        for (auto target : pattern.targets) std::printf(" 0x%llx", target);
        std::puts("");
    }
}

inline Address resolve(const Reader& vmm, DWORD session, bool diagnostics = false,
                       bool allowPartial = false, bool* usedPartial = nullptr) {
    const auto [csrss, winlogon] = proxies(vmm, session);
    const DWORD pid = csrss | kKernel;
    if (diagnostics) std::printf("Session %lu: csrss=%lu winlogon=%lu\n", session, csrss, winlogon);
    // Compare Mambo's patterns against the instruction-specific references.
    const int x = kWildcard;
    std::vector<Pattern> slots{
        {"Mambo short A", {0x48,0x8b,0x05,x,x,x,x,0x48,0x8b,0x04,0xc8}, 3, true, {}},
        {"Mambo short B", {0x48,0x8b,0x05,x,x,x,x,0xff,0xc9}, 3, true, {}},
        {"memflow slots", {0x48,0x8b,0x05,x,x,x,x,0xff,0xc9,0x48,0x8b,0x04,0xc8,0xc3}, 3, true, {}},
    };
    std::vector<Pattern> offsets{
        {"Mambo offset", {0x48,0x8d,0x90,x,x,x,x,0xe8,x,x,x,x,0x0f,0x57,0xc0}, 3, false, {}},
        {"memflow offset A", {0xb9,0x00,0x80,0xff,0xff,x,0x22,0xb4,x,x,x,x,x,0x41}, 9, false, {}},
        {"memflow offset B", {0x40,0x0f,0xb6,x,0x48,0xc1,0xe9,0x02,0x8a,0x94,0x08,x,x,x,x}, 11, false, {}},
    };
    const DWORD reader = winlogon | kKernel;
    const unsigned missing = scan(vmm, pid, reader, "win32k.sys", slots) +
                             scan(vmm, pid, reader, "win32kbase.sys", offsets);
    if (usedPartial) *usedPartial = missing != 0;
    if (diagnostics) {
        std::printf("Unread executable pages: %u\n", missing);
        printMatches("win32k.sys", slots);
        printMatches("win32kbase.sys", offsets);
    }
    if (missing && !allowPartial) throw std::runtime_error("Executable sections contain unreadable pages.");
    const auto& global = slots.back().targets;
    const auto& mambo = offsets.front().targets;
    const auto& first = offsets[1].targets;
    const auto& second = offsets[2].targets;
    if (global.size() != 1 || mambo.size() != 1 || first.size() + second.size() != 1 ||
        mambo.front() != (first.empty() ? second.front() : first.front()))
        throw std::runtime_error("No unique, corroborated candidate.");
    uint64_t table = 0, slot = 0, state = 0;
    // memflow's slot-zero chain is a hypothesis to test, even when readable.
    if (!vmm.exact(pid, global.front(), table) || !kernelAddress(table) ||
        !vmm.exact(pid, table, slot) || !kernelAddress(slot) ||
        !vmm.exact(pid, slot, state) || !kernelAddress(state) ||
        state > UINT64_MAX - mambo.front() - sizeof(Bitmap))
        throw std::runtime_error("Slot-zero pointer chain unavailable.");
    Bitmap buffer{};
    const uint64_t address = state + mambo.front();
    if (!vmm.exact(reader, address, buffer)) throw std::runtime_error("Candidate is unreadable in winlogon.");
    return {reader, address};
}

}  // namespace dma_keyboard
