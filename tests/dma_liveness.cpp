#include "../src/process.h"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
    char executable[MAX_PATH];GetModuleFileNameA(nullptr,executable,MAX_PATH);
    const std::string path=std::string(executable).substr(0,std::string(executable).find_last_of("\\/"));
    const auto dll=LoadLibraryA((path+"\\vmm.dll").c_str());assert(dll);
    const auto mode=reinterpret_cast<void (*)(int)>(GetProcAddress(dll,"TestMode"));assert(mode);
    const auto layoutLookups=reinterpret_cast<unsigned (*)()>(GetProcAddress(dll,"TestLayoutLookups"));
    const auto memoryReads=reinterpret_cast<unsigned (*)()>(GetProcAddress(dll,"TestMemoryReads"));
    assert(layoutLookups && memoryReads);
    auto process=makeDmaProcess("test-only",path,true);
    std::string error;assert(process->attach("DayZ_x64.exe",error));
    uint16_t mz=0;uint64_t world=0;
    assert(!process->read(process->module().base,mz));
    assert(process->read(process->module().base+0xabc0,world) && world==0x200000);
    // Reproduces the old watchdog contradiction: header fails, game data succeeds.
    for (int i=0;i<8;++i) assert(process->alive());
    assert(std::string(process->aliveDetail()).find("uncached transport OK")!=std::string::npos);
    // Keyboard startup failure must leave the working transport available.
    const unsigned readsBeforeKeyboardCheck=memoryReads();
    assert(!process->prepareRemoteKeys(error));
    assert(error.find("26200.9974")!=std::string::npos && layoutLookups()==0);
    assert(error.find("26200.9711")!=std::string::npos);
    assert(!process->remoteKeySample().pid);
    process->maintain();
    assert(memoryReads()==readsBeforeKeyboardCheck);
    assert(!process->remoteKeySample().pid && process->alive());
    mode(11);
    assert(!process->prepareRemoteKeys(error));
    assert(error.find("Invalid target Windows build number")!=std::string::npos && layoutLookups()==0);
    mode(13);
    assert(!process->prepareRemoteKeys(error));
    assert(error.find("Could not read target Windows revision")!=std::string::npos && layoutLookups()==0);
    mode(12);
    assert(!process->prepareRemoteKeys(error)); // The mock deliberately exposes no layout.
    assert(error.find("keyboard layout lookup")!=std::string::npos && layoutLookups()>0);
    mode(0);
    for (int failure : {1,2,3,4,5,6,7}) {
        mode(failure);
        for (int i=0;i<4;++i) assert(!process->alive());
        mode(0);assert(process->alive());
    }
    // Absence of a kernel identity never turns mere metadata into success.
    process.reset();mode(8);process=makeDmaProcess("test-only",path,true);
    assert(process->attach("DayZ_x64.exe",error));assert(!process->alive());
    mode(9);assert(process->alive()); // Existing PE fallback still works.
    mode(1);assert(!process->alive());
    process.reset();
    mode(10);
    process=makeDmaProcess("test-only",path,true);
    assert(!process->attach("DayZ_x64.exe",error));
    assert(!process->prepareRemoteKeys(error) && !process->remoteKeySample().pid && layoutLookups()==0);
    process.reset();FreeLibrary(dll);
    puts("PASS DMA liveness and keyboard setup: missing hardware, live reads, disconnect/recovery, version gate and invalid version data");
}
