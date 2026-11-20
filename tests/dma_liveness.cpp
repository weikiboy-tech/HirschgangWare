#include "../src/process.h"
#include <cassert>
#include <cstdio>
#include <string>

int main() {
    char executable[MAX_PATH];GetModuleFileNameA(nullptr,executable,MAX_PATH);
    const std::string path=std::string(executable).substr(0,std::string(executable).find_last_of("\\/"));
    const auto dll=LoadLibraryA((path+"\\vmm.dll").c_str());assert(dll);
    const auto mode=reinterpret_cast<void (*)(int)>(GetProcAddress(dll,"TestMode"));assert(mode);
    auto process=makeDmaProcess("test-only",path,true);
    std::string error;assert(process->attach("DayZ_x64.exe",error));
    uint16_t mz=0;uint64_t world=0;
    assert(!process->read(process->module().base,mz));
    assert(process->read(process->module().base+0xabc0,world) && world==0x200000);
    // Reproduces the old watchdog contradiction: header fails, game data succeeds.
    for (int i=0;i<8;++i) assert(process->alive());
    assert(std::string(process->aliveDetail()).find("uncached transport OK")!=std::string::npos);
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
    process.reset();FreeLibrary(dll);
    puts("PASS DMA liveness: unreadable PE with live data, disconnect with cached metadata, exit, identity change, API failures, short reads and legacy fallback");
}
