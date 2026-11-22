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
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
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
        if (!fn) throw std::runtime_error(std::string("Missing VMM export: ") + name);
        return fn;
    }

    void open(const wchar_t* path, bool printTransportErrors = false) {
        dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) throw std::runtime_error("Could not load vmm.dll. Pass its absolute path.");
        close = load<decltype(close)>("VMMDLL_Close");
        const auto initialize = load<decltype(&VMMDLL_Initialize)>("VMMDLL_Initialize");
        LPCSTR args[]{"", "-device", "fpga", "-waitinitialize", "-printf"};
        handle = initialize(DWORD(std::size(args) - (printTransportErrors ? 0 : 1)), args);
        if (!handle) throw std::runtime_error(printTransportErrors
            ? "VMM FPGA initialization failed. See the MemProcFS/LeechCore output."
            : "VMM FPGA initialization failed. Run --transport-only for diagnostics.");
    }

    void loadVersionApi() {
        registry = load<decltype(registry)>("VMMDLL_WinReg_QueryValueExU");
    }

    void loadLayoutApi() {
        read = load<decltype(read)>("VMMDLL_MemReadEx");
        module = load<decltype(module)>("VMMDLL_ProcessGetModuleBaseU");
        processes = load<decltype(processes)>("VMMDLL_ProcessGetInformationAll");
        free = load<decltype(free)>("VMMDLL_MemFree");
        exportAddress = load<decltype(exportAddress)>("VMMDLL_ProcessGetProcAddressU");
    }

    ~Vmm() {
        if (handle) close(handle);
        if (dll) FreeLibrary(dll);
    }

};

int checkTransport(const wchar_t* vmmPath) {
    const auto driverPath = std::filesystem::path(vmmPath).parent_path() / L"FTD3XX.dll";
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> driver(
        LoadLibraryExW(driverPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH), FreeLibrary);
    if (!driver) throw std::runtime_error("Could not load FTD3XX.dll beside vmm.dll (Win32 " +
                                          std::to_string(GetLastError()) + ").");

    // FTDI AN_379 defines the API and FT_FLAGS_OPENED for D3XX USB bridges.
    using ListDevices = ULONG (WINAPI*)(LPDWORD);
    using DeviceDetails = ULONG (WINAPI*)(DWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PVOID, PVOID, PHANDLE);
    const auto list = reinterpret_cast<ListDevices>(GetProcAddress(driver.get(), "FT_CreateDeviceInfoList"));
    const auto details = reinterpret_cast<DeviceDetails>(GetProcAddress(driver.get(), "FT_GetDeviceInfoDetail"));
    if (!list || !details) throw std::runtime_error("Required FTDI D3XX enumeration API unavailable.");

    DWORD count = 0;
    const ULONG listStatus = list(&count);
    if (listStatus != 0) throw std::runtime_error("FT_CreateDeviceInfoList failed (D3XX status " +
                                                 std::to_string(listStatus) + ").");
    std::printf("FTDI D3XX devices: %lu\n", count);
    if (!count) throw std::runtime_error("No D3XX bridge detected.");

    constexpr DWORD openedFlag = 1;
    DWORD flags = 0, type = 0, id = 0, location = 0;
    char serial[16]{}, description[32]{};
    HANDLE openedHandle = nullptr;
    const ULONG detailStatus = details(0, &flags, &type, &id, &location, serial, description, &openedHandle);
    if (detailStatus != 0) throw std::runtime_error("FT_GetDeviceInfoDetail failed (D3XX status " +
                                                   std::to_string(detailStatus) + ").");
    if (flags & openedFlag) {
        std::puts("D3XX reports device 0 as open. VMM initialization skipped.");
        std::puts("If capture is stopped, retry from the same Windows context as the launcher.");
        return 5;
    }

    std::printf("D3XX device 0 type %lu is available. Opening the VMM FPGA session.\n", type);
    Vmm vmm;
    vmm.open(vmmPath, true);
    std::puts("VMM FPGA session initialized. Windows keyboard compatibility was not checked.");
    return 0;
}

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
    const bool transportOnly = argc == 3 && wcscmp(argv[2], L"--transport-only") == 0;
    const bool compatibilityOnly = argc == 3 && wcscmp(argv[2], L"--compatibility-only") == 0;
    if (argc < 2 || argc > 4 || (argc == 4 && wcscmp(argv[3], L"--verify") != 0)) {
        std::puts("Usage: keyboard_probe.exe <absolute path to vmm.dll> --transport-only");
        std::puts("Usage: keyboard_probe.exe <absolute path to vmm.dll> --compatibility-only");
        std::puts("Usage: keyboard_probe.exe <absolute path to vmm.dll> [target session, default 1] [--verify]");
        return 1;
    }
    if (transportOnly) return checkTransport(argv[1]);
    Vmm vmm;
    vmm.open(argv[1]);
    vmm.loadVersionApi();
    const auto [build, revision] = vmm.version();
    std::printf("Target Windows: %lu.%lu\n", build, revision);
    if (!verifiedVersion(build, revision)) {
        std::printf("Build check: unverified Windows build. Verified build: %lu.%lu. "
                    "Keyboard layout lookup and sampling skipped.\n", kVerifiedBuild, kVerifiedRevision);
        return 4;
    }
    if (compatibilityOnly) {
        std::puts("Build check: matches the verified version. Keyboard layout and sampling were not tested.");
        return 0;
    }
    const DWORD session = argc >= 3 ? std::stoul(argv[2]) : 1;
    vmm.loadLayoutApi();
    const auto candidate = resolve(vmm, session, true, true);
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
