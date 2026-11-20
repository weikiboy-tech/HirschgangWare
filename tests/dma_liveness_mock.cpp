#define NOMINMAX
#include <windows.h>
#include <cstdlib>
#include <cstring>
#pragma warning(push)
#pragma warning(disable: 4200 4201 4324)
extern "C" {
#include "../third_party/pcileech/vmmdll.h"
}
#pragma warning(pop)

// Test DLL only; never copied into the application's DMA directory.
static int mode=0;
constexpr ULONG64 image=0x140000000ull, object=0xffff800000001000ull;
extern "C" void TestMode(int value) { mode=value; }
extern "C" VMM_HANDLE VMMDLL_Initialize(DWORD,LPCSTR[]) { return reinterpret_cast<VMM_HANDLE>(1); }
extern "C" VOID VMMDLL_Close(VMM_HANDLE) {}
extern "C" VOID VMMDLL_MemFree(PVOID p) { free(p); }
extern "C" BOOL VMMDLL_PidGetFromName(VMM_HANDLE,LPCSTR,PDWORD pid) { *pid=1234;return TRUE; }
extern "C" BOOL VMMDLL_Map_GetModuleFromNameU(VMM_HANDLE,DWORD,LPCSTR,PVMMDLL_MAP_MODULEENTRY* result,DWORD) {
    *result=static_cast<PVMMDLL_MAP_MODULEENTRY>(calloc(1,sizeof(VMMDLL_MAP_MODULEENTRY)));
    (*result)->vaBase=image;(*result)->cbImageSize=0x100000;
    return TRUE;
}
extern "C" BOOL VMMDLL_Map_GetVadU(VMM_HANDLE,DWORD,BOOL,PVMMDLL_MAP_VAD*) { return FALSE; }
extern "C" BOOL VMMDLL_ConfigSet(VMM_HANDLE,ULONG64 option,ULONG64) {
    if ((option & 0xffffffff00000000ull)==VMMDLL_OPT_REFRESH_SPECIFIC_PROCESS) return mode!=6;
    return TRUE;
}
extern "C" BOOL VMMDLL_ProcessGetInformation(VMM_HANDLE,DWORD,PVMMDLL_PROCESS_INFORMATION info,PSIZE_T size) {
    if (mode==5) return FALSE;
    if (*size!=sizeof(*info) || info->magic!=VMMDLL_PROCESS_INFORMATION_MAGIC) return FALSE;
    info->dwPID=mode==3 ? 999 : 1234;
    info->dwState=mode==2 ? 1 : 0;
    info->win.vaEPROCESS=mode==8 ? 0 : mode==4 ? object+0x1000 : object;
    *size=sizeof(*info);return TRUE;
}
extern "C" BOOL VMMDLL_MemReadEx(VMM_HANDLE,DWORD pid,ULONG64 address,PBYTE bytes,DWORD length,PDWORD got,ULONG64 flags) {
    *got=0;
    if (mode==1) return FALSE; // DLL still returns cached active process metadata.
    if (pid==4 && address==object && length==16) {
        if (!(flags & VMMDLL_FLAG_NOCACHE) || !(flags & VMMDLL_FLAG_NOPAGING)) return FALSE;
        memset(bytes,0x42,length);*got=mode==7 ? 8 : length;return TRUE;
    }
    if (pid==1234 && address==image+0xabc0 && length==8) {
        ULONG64 world=0x200000;memcpy(bytes,&world,8);*got=8;return TRUE;
    }
    if (mode==9 && pid==1234 && address==image && length==2) {
        bytes[0]='M';bytes[1]='Z';*got=2;return TRUE;
    }
    return FALSE; // PE header/probes unavailable, even with readable game data.
}
