#define NOMINMAX
#include <windows.h>
#pragma warning(push)
#pragma warning(disable : 4200 4201)
#include "../src/keyboard_resolver.h"
#pragma warning(pop)

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace dma_keyboard;

struct Vmm : Reader {
    HMODULE dll = nullptr;
    decltype(&VMMDLL_Close) close = nullptr;
    decltype(&VMMDLL_ProcessGetProcAddressU) exportAddress = nullptr;

    template<class Fn>
    Fn load(const char* name) const {
        const auto fn = reinterpret_cast<Fn>(GetProcAddress(dll, name));
        if (!fn) throw std::runtime_error(name);
        return fn;
    }

    void open(const wchar_t* path) {
        dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) throw std::runtime_error("Could not load vmm.dll. Pass its absolute path.");
        close = load<decltype(close)>("VMMDLL_Close");
        read = load<decltype(read)>("VMMDLL_MemReadEx");
        module = load<decltype(module)>("VMMDLL_ProcessGetModuleBaseU");
        processes = load<decltype(processes)>("VMMDLL_ProcessGetInformationAll");
        free = load<decltype(free)>("VMMDLL_MemFree");
        exportAddress = load<decltype(exportAddress)>("VMMDLL_ProcessGetProcAddressU");
        registry = load<decltype(registry)>("VMMDLL_WinReg_QueryValueExU");
        const auto initialize = load<decltype(&VMMDLL_Initialize)>("VMMDLL_Initialize");
        LPCSTR args[]{"", "-device", "fpga", "-waitinitialize"};
        handle = initialize(DWORD(std::size(args)), args);
        if (!handle) throw std::runtime_error("FPGA initialization failed. Close other DMA clients and check the link.");
    }

    ~Vmm() {
        if (handle) close(handle);
        if (dll) FreeLibrary(dll);
    }

};

bool phase(const Vmm& vmm, DWORD pid, uint64_t address, const char* prompt, unsigned expected, bool localControl = false) {
    std::printf("\n%s\nPress Enter on the second PC, wait for GO, then follow the instruction for 8 seconds.\n", prompt);
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("Interactive verification cancelled.");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    Bitmap previous{};
    if (!vmm.exact(pid, address, previous)) throw std::runtime_error("Baseline read failed.");
    for (unsigned vk = 8; vk < 255; ++vk)
        if (down(previous, vk)) throw std::runtime_error("Baseline contains a held keyboard VK.");
    std::puts("GO");
    std::fflush(stdout);
    unsigned rises = 0, falls = 0, unexpected = 0, mouseChanges = 0, localRises = 0, localFalls = 0;
    bool localWasDown = false;
    const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < stop) {
        Bitmap current{};
        if (!vmm.exact(pid, address, current)) throw std::runtime_error("Read lost during verification.");
        for (unsigned vk = 1; vk < 255; ++vk) {
            const bool now = down(current, vk);
            if (now == down(previous, vk)) continue;
            if (vk == 1 || vk == 2 || vk == 4 || vk == 5 || vk == 6) ++mouseChanges;
            if (vk >= 8 && vk != expected) ++unexpected;
            if (vk == expected) (now ? rises : falls)++;
        }
        previous = current;
        if (localControl) {
            const bool localDown = (GetAsyncKeyState('E') & 0x8000) != 0;
            if (localDown != localWasDown) (localDown ? localRises : localFalls)++;
            localWasDown = localDown;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    const unsigned required = expected ? 2 : 0;
    if (localControl) std::printf("Local E control: rises=%u falls=%u\n", localRises, localFalls);
    const bool passed = rises == required && falls == required && unexpected == 0 &&
                        (!localControl || (localRises == 2 && localFalls == 2));
    std::printf("rises=%u falls=%u unrelated keyboard edges=%u mouse edges=%u: %s\n",
                rises, falls, unexpected, mouseChanges, passed ? "PASS" : "FAIL");
    return passed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) try {
    if (argc < 2 || argc > 4 || (argc == 4 && wcscmp(argv[3], L"--verify") != 0)) {
        std::puts("Usage: keyboard_probe.exe <absolute path to vmm.dll> [target session, default 1] [--verify]");
        return 1;
    }
    const DWORD session = argc >= 3 ? std::stoul(argv[2]) : 1;
    Vmm vmm;
    vmm.open(argv[1]);
    const auto [build, revision] = vmm.version();
    std::printf("Target Windows: %lu.%lu\n", build, revision);
    const auto candidate = resolve(vmm, session, true);
    const auto [reader, address] = candidate;
    const uint64_t oldExport = vmm.exportAddress(vmm.handle, reader, "win32kbase.sys", "gafAsyncKeyState");
    std::printf("Legacy export: 0x%llx\n", oldExport);
    std::printf("Candidate 0x%llx: complete 64-byte read; input unverified.\n", address);
    if (argc != 4) return 0;
    bool ok = phase(vmm, reader, address, "Idle on both PCs.", 0);
    ok &= phase(vmm, reader, address, "Tap E twice on the main keyboard, holding briefly each time.", 'E');
    ok &= phase(vmm, reader, address, "Tap W twice on the main keyboard, holding briefly each time.", 'W');
    ok &= phase(vmm, reader, address, "Move and click the main PC mouse repeatedly.", 0);
    ok &= phase(vmm, reader, address, "Tap E twice on the second PC keyboard, holding briefly each time.", 0, true);
    std::puts(ok ? "Observed the expected aggregate keyboard edges. Physical device identity remains unavailable."
                 : "Candidate failed controlled input verification.");
    return ok ? 0 : 3;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
}
