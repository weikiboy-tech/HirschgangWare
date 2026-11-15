// keyprobe.cpp - standalone DMA keyboard locator/verifier for gafAsyncKeyState.
// Ground truth comes from the MAKCU/box (km.left/right/middle). Symbol independent.
//
// Build: tools\build_keyprobe.bat   (produces tools\keyprobe.exe)
// Usage: keyprobe.exe [probe|scan|scanall|env|ekey|find] [--dll DIR] [--port COM3] [--module win32kbase.sys] [--ctx N]
//   find = automatic read-only discriminator: idle-stability vs. box pulses (no human needed)
//
// The DMA card + COM port must be free (no Ickza.exe / MemProcFS running).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4200 4201 4324)
#include <vmmdll.h>
#pragma warning(pop)

// ------------------------------------------------------------------ logging
static FILE* gLog = nullptr;
static void lg(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof b, _TRUNCATE, fmt, ap);
    va_end(ap);
    std::printf("%s\n", b);
    std::fflush(stdout);
    if (gLog) {
        std::fprintf(gLog, "%s\n", b);
        std::fflush(gLog);
    }
}

// ------------------------------------------------------------------ vmm.dll
#define FN(ret, name, ...) typedef ret(*Pfn_##name)(__VA_ARGS__); static Pfn_##name g##name = nullptr;
FN(VMM_HANDLE, VMMDLL_Initialize, DWORD, LPCSTR*)
FN(VOID, VMMDLL_Close, VMM_HANDLE)
FN(VOID, VMMDLL_MemFree, PVOID)
FN(BOOL, VMMDLL_MemReadEx, VMM_HANDLE, DWORD, ULONG64, PBYTE, DWORD, PDWORD, ULONG64)
FN(BOOL, VMMDLL_PidList, VMM_HANDLE, PDWORD, PSIZE_T)
FN(BOOL, VMMDLL_ProcessGetInformation, VMM_HANDLE, DWORD, PVMMDLL_PROCESS_INFORMATION, PSIZE_T)
FN(BOOL, VMMDLL_ProcessGetInformationAll, VMM_HANDLE, PVMMDLL_PROCESS_INFORMATION*, PDWORD)
FN(QWORD, VMMDLL_ProcessGetModuleBaseU, VMM_HANDLE, DWORD, LPCSTR)
FN(BOOL, VMMDLL_Map_GetModuleFromNameU, VMM_HANDLE, DWORD, LPCSTR, PVMMDLL_MAP_MODULEENTRY*, DWORD)
FN(BOOL, VMMDLL_Map_GetModuleU, VMM_HANDLE, DWORD, PVMMDLL_MAP_MODULE*, DWORD)
FN(BOOL, VMMDLL_Map_GetEATU, VMM_HANDLE, DWORD, LPCSTR, PVMMDLL_MAP_EAT*)
FN(BOOL, VMMDLL_PdbLoad, VMM_HANDLE, DWORD, ULONG64, LPSTR)
FN(BOOL, VMMDLL_PdbSymbolAddress, VMM_HANDLE, LPCSTR, LPCSTR, PQWORD)
FN(BOOL, VMMDLL_ConfigGet, VMM_HANDLE, ULONG64, PULONG64)
#undef FN

// merge the loaded pointers into the API names used below (header declares them as functions,
// so a same-named variable would clash - these macros redirect every call site to the pointer)
#define VMMDLL_Initialize gVMMDLL_Initialize
#define VMMDLL_Close gVMMDLL_Close
#define VMMDLL_MemFree gVMMDLL_MemFree
#define VMMDLL_MemReadEx gVMMDLL_MemReadEx
#define VMMDLL_PidList gVMMDLL_PidList
#define VMMDLL_ProcessGetInformation gVMMDLL_ProcessGetInformation
#define VMMDLL_ProcessGetInformationAll gVMMDLL_ProcessGetInformationAll
#define VMMDLL_ProcessGetModuleBaseU gVMMDLL_ProcessGetModuleBaseU
#define VMMDLL_Map_GetModuleFromNameU gVMMDLL_Map_GetModuleFromNameU
#define VMMDLL_Map_GetModuleU gVMMDLL_Map_GetModuleU
#define VMMDLL_Map_GetEATU gVMMDLL_Map_GetEATU
#define VMMDLL_PdbLoad gVMMDLL_PdbLoad
#define VMMDLL_PdbSymbolAddress gVMMDLL_PdbSymbolAddress
#define VMMDLL_ConfigGet gVMMDLL_ConfigGet

static HMODULE gDll = nullptr;
static VMM_HANDLE gVmm = nullptr;

static bool loadVmm(const std::string& dir) {
    const char* cands[] = {"\\vmm.dll", "\\dma\\vmm.dll"};
    std::string tried;
    for (const char* c : cands) {
        std::string p = dir + c;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            tried += p + " ";
            continue;
        }
        gDll = LoadLibraryExA(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (gDll) {
            lg("[ok] loaded %s", p.c_str());
            break;
        }
    }
    if (!gDll) gDll = LoadLibraryA("vmm.dll");
    if (!gDll) {
        lg("[!!] vmm.dll not found (tried %s)", tried.c_str());
        return false;
    }
    auto sym = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(gDll, n)); };
    VMMDLL_Initialize = (Pfn_VMMDLL_Initialize)sym("VMMDLL_Initialize");
    VMMDLL_Close = (Pfn_VMMDLL_Close)sym("VMMDLL_Close");
    VMMDLL_MemFree = (Pfn_VMMDLL_MemFree)sym("VMMDLL_MemFree");
    VMMDLL_MemReadEx = (Pfn_VMMDLL_MemReadEx)sym("VMMDLL_MemReadEx");
    VMMDLL_PidList = (Pfn_VMMDLL_PidList)sym("VMMDLL_PidList");
    VMMDLL_ProcessGetInformation = (Pfn_VMMDLL_ProcessGetInformation)sym("VMMDLL_ProcessGetInformation");
    VMMDLL_ProcessGetInformationAll = (Pfn_VMMDLL_ProcessGetInformationAll)sym("VMMDLL_ProcessGetInformationAll");
    VMMDLL_ProcessGetModuleBaseU = (Pfn_VMMDLL_ProcessGetModuleBaseU)sym("VMMDLL_ProcessGetModuleBaseU");
    VMMDLL_Map_GetModuleFromNameU = (Pfn_VMMDLL_Map_GetModuleFromNameU)sym("VMMDLL_Map_GetModuleFromNameU");
    VMMDLL_Map_GetModuleU = (Pfn_VMMDLL_Map_GetModuleU)sym("VMMDLL_Map_GetModuleU");
    VMMDLL_Map_GetEATU = (Pfn_VMMDLL_Map_GetEATU)sym("VMMDLL_Map_GetEATU");
    VMMDLL_PdbLoad = (Pfn_VMMDLL_PdbLoad)sym("VMMDLL_PdbLoad");
    VMMDLL_PdbSymbolAddress = (Pfn_VMMDLL_PdbSymbolAddress)sym("VMMDLL_PdbSymbolAddress");
    VMMDLL_ConfigGet = (Pfn_VMMDLL_ConfigGet)sym("VMMDLL_ConfigGet");
    if (!VMMDLL_Initialize || !VMMDLL_MemReadEx || !VMMDLL_PidList) {
        lg("[!!] vmm.dll missing required exports");
        return false;
    }
    return true;
}
// ------------------------------------------------------------------ memory
static const ULONG64 FL = VMMDLL_FLAG_NOCACHE;

static bool rd(DWORD ctx, ULONG64 va, void* out, DWORD cb, bool zeropad = false) {
    DWORD got = 0;
    ULONG64 f = FL | (zeropad ? VMMDLL_FLAG_ZEROPAD_ON_FAIL : 0);
    return VMMDLL_MemReadEx(gVmm, ctx, va, (PBYTE)out, cb, &got, f) && got == cb;
}
static std::string hx(const uint8_t* p, int n) {
    std::string s;
    char b[4];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02X", p[i]);
        s += b;
    }
    return s;
}
static uint64_t modBase(DWORD ctx, const char* name) {
    if (!VMMDLL_ProcessGetModuleBaseU) return 0;
    return VMMDLL_ProcessGetModuleBaseU(gVmm, ctx, name);
}

// ------------------------------------------------------------------ serial (MAKCU)
class Serial {
public:
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open(const char* port) {
        std::string p = "\\\\.\\" + std::string(port);
        h = CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            lg("[!!] can't open %s (err %lu) - is the box in use?", p.c_str(), GetLastError());
            return false;
        }
        DCB dcb{};
        dcb.DCBlength = sizeof dcb;
        if (!GetCommState(h, &dcb)) return false;
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(h, &dcb);
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = 10;
        to.ReadTotalTimeoutConstant = 40;
        to.WriteTotalTimeoutConstant = 60;
        SetCommTimeouts(h, &to);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }
    void send(const char* s) {
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile(h, s, (DWORD)std::strlen(s), &w, nullptr);
    }
    std::string drain() {
        if (h == INVALID_HANDLE_VALUE) return {};
        std::string out;
        char b[512];
        DWORD r = 0;
        while (ReadFile(h, b, sizeof b, &r, nullptr) && r) out.append(b, r);
        return out;
    }
    void close() {
        if (h != INVALID_HANDLE_VALUE) {
            send("km.left(0)\r\n");            // never leave a button hanging
            Sleep(70);
            send("km.left(0)\r\n");
            Sleep(70);
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }
};
static Serial gBox;

static const int DWELL = 150;  // ms after each box command (>=90ms avoids stale reads)

// ground-truth capture: 128 bytes at ctx:va for each phase
struct Samples {
    std::vector<uint8_t> idle, lmb, rmbb, mmbb, idle2;
    bool ok = false;
};
static bool grab(DWORD ctx, ULONG64 va, size_t n, Samples& s) {
    s.idle.assign(n, 0);
    s.lmb.assign(n, 0);
    s.rmbb.assign(n, 0);
    s.mmbb.assign(n, 0);
    s.idle2.assign(n, 0);
    if (!rd(ctx, va, s.idle.data(), (DWORD)n, true)) return false;
    gBox.send("km.left(1)\r\n");
    Sleep(DWELL);
    rd(ctx, va, s.lmb.data(), (DWORD)n, true);
    gBox.send("km.left(0)\r\n");
    Sleep(DWELL);
    rd(ctx, va, s.idle2.data(), (DWORD)n, true);
    gBox.send("km.right(1)\r\n");
    Sleep(DWELL);
    rd(ctx, va, s.rmbb.data(), (DWORD)n, true);
    gBox.send("km.right(0)\r\n");
    Sleep(DWELL);
    gBox.send("km.middle(1)\r\n");
    Sleep(DWELL);
    rd(ctx, va, s.mmbb.data(), (DWORD)n, true);
    gBox.send("km.middle(0)\r\n");
    Sleep(DWELL);
    return true;
}
static inline int bit(const std::vector<uint8_t>& v, int o, int b) { return (v[o] >> b) & 1; }
// ------------------------------------------------------------------ PE export + pdb
static bool eatRva(DWORD ctx, ULONG64 base, const char* want, ULONG64& rva) {
    uint8_t dos[0x40];
    if (!rd(ctx, base, dos, sizeof dos)) return false;
    if (dos[0] != 'M' || dos[1] != 'Z') return false;
    uint32_t pe = 0;
    std::memcpy(&pe, dos + 0x3C, 4);
    if (pe < 0x40 || pe > 0x1000) return false;
    uint8_t nt[0x110];
    if (!rd(ctx, base + pe, nt, sizeof nt)) return false;
    if (nt[0] != 'P' || nt[1] != 'E') return false;
    uint32_t expRva = 0, expSz = 0;
    std::memcpy(&expRva, nt + 0x88, 4);
    std::memcpy(&expSz, nt + 0x8C, 4);
    if (!expRva || expRva > 0x100000) return false;
    uint8_t ed[0x28];
    if (!rd(ctx, base + expRva, ed, sizeof ed)) return false;
    uint32_t nNames = 0, aFuncs = 0, aNames = 0, aOrds = 0;
    std::memcpy(&nNames, ed + 0x18, 4);
    std::memcpy(&aFuncs, ed + 0x1C, 4);
    std::memcpy(&aNames, ed + 0x20, 4);
    std::memcpy(&aOrds, ed + 0x24, 4);
    if (!nNames || nNames > 0x20000) return false;
    for (uint32_t i = 0; i < nNames; ++i) {
        uint32_t nr = 0;
        if (!rd(ctx, base + aNames + i * 4, &nr, 4)) continue;
        char nm[128] = {};
        rd(ctx, base + nr, nm, sizeof nm - 1);
        if (std::strstr(nm, want)) {
            uint16_t ord = 0;
            if (!rd(ctx, base + aOrds + i * 2, &ord, 2)) continue;
            uint32_t fr = 0;
            if (!rd(ctx, base + aFuncs + ord * 4, &fr, 4)) continue;
            rva = fr;
            return true;
        }
    }
    return false;
}
static bool pdbRva(DWORD ctx, ULONG64 base, const char* sym, ULONG64& rva, std::string& mod) {
    if (!VMMDLL_PdbLoad || !VMMDLL_PdbSymbolAddress) return false;
    char m[MAX_PATH] = {};
    if (!VMMDLL_PdbLoad(gVmm, ctx, base, m)) return false;
    QWORD va = 0;
    if (!VMMDLL_PdbSymbolAddress(gVmm, m, sym, &va) || va <= base) return false;
    mod = m;
    rva = va - base;
    return true;
}

// ------------------------------------------------------------------ image + layouts
struct Layout {
    const char* name;
    int o1, b1, o2, b2, o4, b4;  // relative (byte,bit) for VK_LBUTTON(1), VK_RBUTTON(2), VK_MBUTTON(4)
    const char* map;             // human formula
};
static const Layout LAY[] = {
    {"twoBitLow", 0, 2, 0, 4, 1, 0, "byte=(vk*2)/8 bit=(vk%4)*2   [win32k gafAsyncKeyState]"},
    {"twoBitHigh", 0, 3, 0, 5, 1, 1, "byte=(vk*2)/8 bit=(vk%4)*2+1"},
    {"oneBit", 0, 1, 0, 2, 0, 4, "byte=vk/8 bit=vk%8 (1 bit/key)"},
    {"perKeyHi", 1, 7, 2, 7, 4, 7, "byte=vk bit=7 (0x80, 1 byte/key)"},
    {"perKeyLo", 1, 0, 2, 0, 4, 0, "byte=vk bit=0"},
    {"stride2Hi", 2, 7, 4, 7, 8, 7, "byte=vk*2 bit=7"},
    {"perByteBit", 1, 1, 2, 2, 4, 4, "byte=vk bit=vk%8"},
};

// vk -> (byte,bit) formulas per layout hypothesis (used to identify the layout from 2 pressed keys)
static int f_twoBitByte(int vk) { return vk / 4; }
static int f_twoBitBit(int vk) { return (vk % 4) * 2; }
static int f_twoBitHiBit(int vk) { return (vk % 4) * 2 + 1; }
static int f_oneBitByte(int vk) { return vk / 8; }
static int f_oneBitBit(int vk) { return vk % 8; }
static int f_perKeyByte(int vk) { return vk; }
static int f_stride2Byte(int vk) { return vk * 2; }
static int f_seven(int) { return 7; }
static int f_zero(int) { return 0; }
static int f_perByteBit(int vk) { return vk % 8; }

struct LayoutF {
    const char* name;
    int (*bytefn)(int);
    int (*bitfn)(int);
    const char* map;
};
static const LayoutF LAYF[] = {
    {"twoBitLow", f_twoBitByte, f_twoBitBit, "byte=(vk*2)/8 bit=(vk%4)*2"},
    {"twoBitHigh", f_twoBitByte, f_twoBitHiBit, "byte=(vk*2)/8 bit=(vk%4)*2+1"},
    {"oneBit", f_oneBitByte, f_oneBitBit, "byte=vk/8 bit=vk%8"},
    {"perKeyHi", f_perKeyByte, f_seven, "byte=vk bit=7 (0x80/byte)"},
    {"perKeyLo", f_perKeyByte, f_zero, "byte=vk bit=0"},
    {"stride2Hi", f_stride2Byte, f_seven, "byte=vk*2 bit=7"},
    {"perByteBit", f_perKeyByte, f_perByteBit, "byte=vk bit=vk%8"},
};

static bool readImg(DWORD ctx, ULONG64 base, size_t size, std::vector<uint8_t>& out) {
    out.assign(size, 0);
    bool any = false;
    for (size_t o = 0; o < size; o += 0x8000) {
        DWORD n = (DWORD)std::min<size_t>(0x8000, size - o);
        DWORD got = 0;
        if (VMMDLL_MemReadEx(gVmm, ctx, base + o, out.data() + o, n, &got, FL) && got) any = true;
    }
    return any;
}

// differential scan of a whole image; finds offsets where a box pulse is tracked 1:1
static bool scanImage(DWORD ctx, ULONG64 base, size_t size, const char* tag) {
    lg("scan %s ctx=0x%lX base=0x%llX size=0x%zX ...", tag, (unsigned long)ctx, (unsigned long long)base, size);
    std::vector<uint8_t> A, B, C, D, E;
    if (!readImg(ctx, base, size, A)) { lg("  read failed"); return false; }
    gBox.send("km.left(1)\r\n");   Sleep(DWELL); readImg(ctx, base, size, B);
    gBox.send("km.left(0)\r\n");   Sleep(DWELL); readImg(ctx, base, size, C);
    gBox.send("km.right(1)\r\n");  Sleep(DWELL); readImg(ctx, base, size, D);
    gBox.send("km.right(0)\r\n");  Sleep(DWELL);
    gBox.send("km.middle(1)\r\n"); Sleep(DWELL); readImg(ctx, base, size, E);
    gBox.send("km.middle(0)\r\n"); Sleep(DWELL);

    {  // coverage: prove the big reads are real (not mostly zeros)
        size_t nzA = 0, dAB = 0, dAC = 0;
        for (size_t i = 0; i < size; ++i) {
            if (A[i]) ++nzA;
            if (A[i] != B[i]) ++dAB;
            if (A[i] != C[i]) ++dAC;
        }
        lg("  coverage nzA=%zu/%zu diffAB=%zu diffAC=%zu", nzA, size, dAB, dAC);
    }
    {  // every byte that changed with any pulse (idle=A, LMB=B, idle=C, RMB=D, MMB=E)
        int shown = 0;
        size_t firstOff = 0;
        for (size_t i = 0; i < size; ++i) {
            if (A[i] == B[i] && A[i] == C[i] && A[i] == D[i] && A[i] == E[i]) continue;
            if (!shown) firstOff = i;
            if (shown < 200) lg("  D %06zX A=%02X LMB=%02X idle2=%02X RMB=%02X MMB=%02X", i, A[i], B[i], C[i], D[i], E[i]);
            ++shown;
        }
        lg("  changed bytes total=%d (first off 0x%zX)", shown, firstOff);
        if (shown) {
            size_t lo = firstOff > 32 ? firstOff - 32 : 0;
            lg("  context A @0x%zX: %s", lo, hx(A.data() + lo, 96).c_str());
            lg("  context B @0x%zX: %s", lo, hx(B.data() + lo, 96).c_str());
        }
    }
    {  // layout-free: bytes with a bit tracking LMB(0-1-0) + RMB(0-1) + MMB(0-1) in one byte
        int shown = 0;
        for (size_t i = 0; i < size && shown < 30; ++i) {
            int lb = -1, rb = -1, mb = -1, nlb = 0, nrb = 0, nmb = 0;
            for (int b = 0; b < 8; ++b) {
                if (!bit(A, (int)i, b) && bit(B, (int)i, b) && !bit(C, (int)i, b)) { lb = b; ++nlb; }
                if (!bit(A, (int)i, b) && bit(D, (int)i, b)) { rb = b; ++nrb; }
                if (!bit(A, (int)i, b) && bit(E, (int)i, b)) { mb = b; ++nmb; }
            }
            if (nlb && nrb && nmb) {
                lg("  RAW off=0x%zX LMBbit=%d(n%d) RMBbit=%d(n%d) MMBbit=%d(n%d)", i, lb, nlb, rb, nrb, mb, nmb);
                ++shown;
            }
        }
    }

    struct Hit { size_t off; int lay; };
    std::vector<Hit> hits;
    for (size_t o = 0; o + 16 < size; ++o) {
        for (int li = 0; li < (int)(sizeof LAY / sizeof LAY[0]); ++li) {
            const Layout& L = LAY[li];
            size_t p1 = o + L.o1, p2 = o + L.o2, p4 = o + L.o4;
            if (p4 >= size) continue;
            if (!bit(A, (int)p1, L.b1) && bit(B, (int)p1, L.b1) && !bit(C, (int)p1, L.b1) &&
                !bit(A, (int)p2, L.b2) && bit(D, (int)p2, L.b2) && !bit(A, (int)p4, L.b4) && bit(E, (int)p4, L.b4)) {
                hits.push_back({o, li});
                break;
            }
        }
    }
    lg("  raw hits=%zu", hits.size());
    bool confirmed = false;
    int lim = (int)std::min<size_t>(hits.size(), 8);
    for (int i = 0; i < lim; ++i) {
        const Layout& L = LAY[hits[i].lay];
        ULONG64 va = base + hits[i].off;
        Samples s;
        if (!grab(ctx, va, 32, s)) { lg("  hit va=0x%llX unreadable", (unsigned long long)va); continue; }
        // confirm the exact triples once more (fresh pattern)
        bool ok1 = !bit(s.idle, L.o1, L.b1) && bit(s.lmb, L.o1, L.b1) && !bit(s.idle2, L.o1, L.b1);
        bool ok2 = !bit(s.idle, L.o2, L.b2) && bit(s.rmbb, L.o2, L.b2);
        bool ok4 = !bit(s.idle, L.o4, L.b4) && bit(s.mmbb, L.o4, L.b4);
        if (ok1 && ok2 && ok4) confirmed = true;
        lg("  %s %-10s array=0x%llX (off 0x%zX) confirm=%d%d%d :: %s", (ok1 && ok2 && ok4) ? "CONFIRM" : "reject ", L.name,
           (unsigned long long)va, hits[i].off, ok1 ? 1 : 0, ok2 ? 1 : 0, ok4 ? 1 : 0, L.map);
        if (ok1) lg("      idle=%s", hx(s.idle.data(), 16).c_str());
    }
    if (hits.empty()) lg("  no layout match found");
    return confirmed;
}

// ------------------------------------------------------------------ processes / ctxs
struct Proc {
    DWORD pid;
    std::string name;
    DWORD session;
};
static std::vector<Proc> enumProcs() {
    std::vector<Proc> out;
    if (VMMDLL_ProcessGetInformationAll) {
        PVMMDLL_PROCESS_INFORMATION all = nullptr;
        DWORD n = 0;
        if (VMMDLL_ProcessGetInformationAll(gVmm, &all, &n) && all && n) {
            for (DWORD i = 0; i < n; ++i) out.push_back({all[i].dwPID, all[i].szName, all[i].win.dwSessionId});
            VMMDLL_MemFree(all);
            return out;
        }
    }
    SIZE_T n = 0;
    if (!VMMDLL_PidList(gVmm, nullptr, &n) || !n) return out;
    std::vector<DWORD> pids(n);
    SIZE_T n2 = n;
    if (!VMMDLL_PidList(gVmm, pids.data(), &n2)) return out;
    pids.resize(n2);
    for (DWORD p : pids) {
        VMMDLL_PROCESS_INFORMATION pi{};
        SIZE_T cb = sizeof pi;
        if (VMMDLL_ProcessGetInformation && VMMDLL_ProcessGetInformation(gVmm, p, &pi, &cb) && pi.szName[0]) {
            out.push_back({p, pi.szName, pi.win.dwSessionId});
        } else {
            out.push_back({p, "?", 0});
        }
    }
    return out;
}
static bool interesting(const std::string& n) { return n == "csrss.exe" || n == "winlogon.exe" || n == "DayZ_x64.exe" || n == "System"; }

static void testCandidate(DWORD ctx, ULONG64 va, const char* tag) {
    Samples s;
    if (!grab(ctx, va, 128, s)) {
        lg("  %-28s ctx=0x%-4lX va=0x%llX  UNREADABLE", tag, (unsigned long)ctx, (unsigned long long)va);
        return;
    }
    std::string lmb, rmb, mmb;
    for (int o = 0; o < 128; ++o)
        for (int b = 0; b < 8; ++b) {
            char t[24];
            if (!bit(s.idle, o, b) && bit(s.lmb, o, b) && !bit(s.idle2, o, b)) {
                std::snprintf(t, sizeof t, "b%d.%d ", o, b);
                lmb += t;
            }
            if (!bit(s.idle, o, b) && bit(s.rmbb, o, b)) {
                std::snprintf(t, sizeof t, "b%d.%d ", o, b);
                rmb += t;
            }
            if (!bit(s.idle, o, b) && bit(s.mmbb, o, b)) {
                std::snprintf(t, sizeof t, "b%d.%d ", o, b);
                mmb += t;
            }
        }
    lg("  %-28s ctx=0x%-4lX va=0x%llX  LMB[%s] RMB[%s] MMB[%s]", tag, (unsigned long)ctx, (unsigned long long)va, lmb.c_str(), rmb.c_str(),
       mmb.c_str());
    if (!lmb.empty()) lg("      idle=%s", hx(s.idle.data(), 32).c_str());
}

// ------------------------------------------------------------------ session space probe
static void sessionProbe(const std::vector<Proc>& procs) {
    lg("--- session-space readability (8-byte probes) ---");
    const ULONG64 addrs[] = {0xFFFFF90000000000ull, 0xFFFFF90000001000ull, 0xFFFFF90080000000ull, 0xFFFFF90080001000ull,
                             0xFFFFF98000000000ull, 0xFFFFF98000001000ull, 0xFFFFF98080000000ull, 0xFFFFF98080001000ull,
                             0xFFFFF97FFFFF0000ull, 0xFFFFFA8000000000ull};
    std::vector<DWORD> ctxs = {4};
    for (const auto& p : procs)
        if (interesting(p.name)) ctxs.push_back(p.pid);
    for (DWORD c : ctxs) {
        std::string line;
        for (ULONG64 a : addrs) {
            uint8_t b[8] = {};
            char t[40];
            if (rd(c, a, b, 8))
                std::snprintf(t, sizeof t, " %llX=%s", (unsigned long long)a, hx(b, 4).c_str());
            else
                std::snprintf(t, sizeof t, " %llX=-", (unsigned long long)a);
            line += t;
        }
        lg("  ctx=0x%lX%s", (unsigned long)c, line.c_str());
    }
}

static std::vector<DWORD> buildCtxs(const std::vector<Proc>& procs) {
    std::vector<DWORD> v;
    auto add = [&](DWORD x) {
        if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
    };
    add(4);
    add(4 | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY);
    for (const auto& p : procs)
        if (interesting(p.name)) {
            add(p.pid);
            add(p.pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY);
        }
    return v;
}

static std::string exeDir() {
    char b[MAX_PATH];
    GetModuleFileNameA(nullptr, b, MAX_PATH);
    std::string s = b;
    return s.substr(0, s.find_last_of("\\/"));
}

static void doProbe(const std::vector<Proc>& procs, bool pulses) {
    ULONG64 v = 0;
    if (VMMDLL_ConfigGet) {
        if (VMMDLL_ConfigGet(gVmm, VMMDLL_OPT_WIN_VERSION_BUILD, &v)) lg("target windows build: %llu", v);
        if (VMMDLL_ConfigGet(gVmm, VMMDLL_OPT_CONFIG_VMM_VERSION_MAJOR, &v)) lg("vmm version: %llu", v);
    }
    lg("--- interesting processes (pid session name) ---");
    for (const auto& p : procs)
        if (interesting(p.name)) lg("  pid=0x%-6lX session=%-3lu %s", (unsigned long)p.pid, (unsigned long)p.session, p.name.c_str());
    sessionProbe(procs);

    const char* mods[] = {"win32kbase.sys", "win32kfull.sys", "win32k.sys", "win32ksgd.sys"};
    lg("--- win32k* module bases per ctx (MZ present?) ---");
    std::vector<DWORD> ctxs = buildCtxs(procs);
    for (DWORD c : ctxs) {
        for (const char* m : mods) {
            uint64_t b = modBase(c, m);
            if (!b) continue;
            uint16_t mz = 0;
            bool ok = rd(c, b, &mz, 2);
            lg("  ctx=0x%-9lX %-14s base=0x%llX read=%d mz=%04X", (unsigned long)c, m, (unsigned long long)b, ok ? 1 : 0, mz);
        }
    }

    lg("--- rva resolution via PDB + export (ctx 4) ---");
    uint64_t rvaKb = 0, rvaKf = 0;
    const ULONG64 baseKb = modBase(4, "win32kbase.sys"), baseKf = modBase(4, "win32kfull.sys");
    if (!pulses) return;
    std::string mod;
    if (baseKb) {
        if (pdbRva(4, baseKb, "gafAsyncKeyState", rvaKb, mod)) lg("  PDB win32kbase(%s) gafAsyncKeyState rva=0x%llX va=0x%llX", mod.c_str(), rvaKb, baseKb + rvaKb);
        else lg("  PDB win32kbase resolve FAILED");
        uint64_t e = 0;
        if (eatRva(4, baseKb, "gafAsyncKeyState", e)) lg("  EAT win32kbase(own parser) rva=0x%llX va=0x%llX", e, baseKb + e);
        else lg("  EAT win32kbase(own parser) FAILED");
        if (VMMDLL_Map_GetEATU) {
            PVMMDLL_MAP_EAT eat = nullptr;
            if (VMMDLL_Map_GetEATU(gVmm, 4, "win32kbase.sys", &eat) && eat) {
                lg("  VMMDLL_Map_GetEATU win32kbase: cMap=%lu names=%lu funcs=%lu vaBase=0x%llX", eat->cMap, eat->cNumberOfNames,
                   eat->cNumberOfFunctions, eat->vaModuleBase);
                VMMDLL_MemFree(eat);
            } else lg("  VMMDLL_Map_GetEATU win32kbase FAILED");
        }
    }
    if (baseKf && pdbRva(4, baseKf, "gafAsyncKeyState", rvaKf, mod)) lg("  PDB win32kfull(%s) rva=0x%llX va=0x%llX", mod.c_str(), rvaKf, baseKf + rvaKf);

    lg("--- ground-truth candidate tests (box pulses on the MAIN pc) ---");
    for (DWORD c : ctxs) {
        uint64_t b = modBase(c, "win32kbase.sys");
        if (b && rvaKb) testCandidate(c, b + rvaKb, "win32kbase+pdb");
        b = modBase(c, "win32kfull.sys");
        if (b && rvaKf) testCandidate(c, b + rvaKf, "win32kfull+pdb");
    }
    testCandidate(4, baseKb + 0x2AE7CD, "win32kbase+0x2AE7CD");
    testCandidate(4, baseKb + 0x2AE2D0, "win32kbase+0x2AE2D0");
}

static bool scanOne(DWORD ctx, const char* module) {
    uint64_t base = modBase(ctx, module);
    if (!base) { lg("scan %s ctx=0x%lX: no base", module, (unsigned long)ctx); return false; }
    uint32_t size = 0x400000;
    if (VMMDLL_Map_GetModuleFromNameU) {
        PVMMDLL_MAP_MODULEENTRY e = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(gVmm, ctx, module, &e, 0) && e) {
            if (e->cbImageSize) size = e->cbImageSize;
            VMMDLL_MemFree(e);
        }
    }
    return scanImage(ctx, base, size, module);
}

// ------------------------------------------------------------------ read-only key-signature search (no box input)
struct Bitc {
    size_t off;
    int bit;
};

static std::vector<Bitc> pressBits(DWORD ctx, ULONG64 base, size_t size, int vk, int rounds) {
    std::unordered_map<size_t, uint8_t> acc;
    bool first = true;
    for (int r = 0; r < rounds; ++r) {
        lg("  Runde %d/%d  Taste 0x%02X: 1 PIEB = DRUECKEN und HALTEN, 2 PIEPS = LOSLASSEN", r + 1, rounds, vk);
        MessageBeep(MB_ICONASTERISK);
        Beep(1100, 300);
        Sleep(1200);
        std::vector<uint8_t> H1, H2, R1, R2;
        readImg(ctx, base, size, H1);
        readImg(ctx, base, size, H2);
        MessageBeep(MB_ICONHAND);
        Beep(650, 130);
        Beep(650, 130);
        Sleep(1200);
        readImg(ctx, base, size, R1);
        readImg(ctx, base, size, R2);
        if (first) {
            for (size_t i = 0; i < size; ++i) {
                uint8_t m = H1[i] & H2[i] & ~R1[i] & ~R2[i];
                if (m) acc[i] = m;
            }
            first = false;
        } else {
            for (auto it = acc.begin(); it != acc.end();) {
                uint8_t m = H1[it->first] & H2[it->first] & ~R1[it->first] & ~R2[it->first];
                it->second &= m;
                if (!it->second)
                    it = acc.erase(it);
                else
                    ++it;
            }
        }
        lg("    Runde %d: %zu Kandidatenbytes uebrig", r + 1, acc.size());
        if (acc.empty()) break;
    }
    std::vector<Bitc> out;
    for (auto& kv : acc)
        for (int b = 0; b < 8; ++b)
            if (kv.second & (1u << b)) out.push_back({kv.first, b});
    std::sort(out.begin(), out.end(), [](const Bitc& a, const Bitc& b) { return a.off < b.off || (a.off == b.off && a.bit < b.bit); });
    return out;
}

static void doEKey(DWORD ctx, const char* module, int k1, int k2, int rounds) {
    uint64_t base = modBase(ctx, module);
    if (!base) {
        lg("ekey: kein Base fuer %s in ctx 0x%lX", module, (unsigned long)ctx);
        return;
    }
    uint32_t size = 0x400000;
    if (VMMDLL_Map_GetModuleFromNameU) {
        PVMMDLL_MAP_MODULEENTRY e = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(gVmm, ctx, module, &e, 0) && e) {
            if (e->cbImageSize) size = e->cbImageSize;
            VMMDLL_MemFree(e);
        }
    }
    lg("=== EKEY read-only: ctx=0x%lX %s base=0x%llX size=0x%X  (NUR auslesen, kein Box-Input) ===", (unsigned long)ctx, module,
       (unsigned long long)base, size);
    lg("=== Du drueckst nur Tasten am Main-PC: 1 PIEB = DRUECKEN+HALTEN, 2 PIEPS = LOSLASSEN ===");

    auto b1 = pressBits(ctx, base, size, k1, rounds);
    lg("  Taste 0x%02X: %zu stabile Bit(s)", k1, b1.size());
    for (const auto& c : b1) lg("    off=0x%zX bit=%d va=0x%llX", c.off, c.bit, (unsigned long long)(base + c.off));

    auto b2 = pressBits(ctx, base, size, k2, rounds);
    lg("  Taste 0x%02X: %zu stabile Bit(s)", k2, b2.size());
    for (const auto& c : b2) lg("    off=0x%zX bit=%d va=0x%llX", c.off, c.bit, (unsigned long long)(base + c.off));

    bool any = false;
    for (const auto& L : LAYF) {
        for (const auto& e : b1) {
            if (e.bit != L.bitfn(k1)) continue;
            int64_t x = (int64_t)e.off - L.bytefn(k1);
            int64_t wOff = x + L.bytefn(k2);
            int wBit = L.bitfn(k2);
            for (const auto& w : b2)
                if ((int64_t)w.off == wOff && w.bit == wBit) {
                    lg("MATCH layout=%s  arrayBase=0x%llX  ctx=0x%lX  module=%s  :: %s", L.name, (unsigned long long)(base + x),
                       (unsigned long)ctx, module, L.map);
                    lg("   -> E(0x45) va=0x%llX bit=%d   W(0x57) va=0x%llX bit=%d", (unsigned long long)(base + x + L.bytefn(0x45)),
                       L.bitfn(0x45), (unsigned long long)(base + x + L.bytefn(0x57)), L.bitfn(0x57));
                    any = true;
                }
        }
    }
    if (!any) lg("KEIN Layout-Match - Kandidaten oben pruefen");
}

// ------------------------------------------------------------------ automatic finder (read-only + box pulses)
// Discriminates counters/timers (unstable while idle) from a key bitmap (stable while idle, reacts to a press).
// Ground truth = MAKCU pulses on the MAIN pc -> no human needed. Read-only: only VMMDLL_MemReadEx.
static void doFind(DWORD ctx, const char* module) {
    const ULONG64 base = modBase(ctx, module);
    if (!base) { lg("find: no base for %s in ctx 0x%lX", module, (unsigned long)ctx); return; }
    uint32_t size = 0x400000;
    if (VMMDLL_Map_GetModuleFromNameU) {
        PVMMDLL_MAP_MODULEENTRY e = nullptr;
        if (VMMDLL_Map_GetModuleFromNameU(gVmm, ctx, module, &e, 0) && e) {
            if (e->cbImageSize) size = e->cbImageSize;
            VMMDLL_MemFree(e);
        }
    }
    lg("=== FIND read-only: ctx=0x%lX %s base=0x%llX size=0x%X ===", (unsigned long)ctx, module, (unsigned long long)base, size);
    std::vector<uint8_t> A, B;
    if (!readImg(ctx, base, size, A)) { lg("read A failed"); return; }
    Sleep(250);
    if (!readImg(ctx, base, size, B)) { lg("read B failed"); return; }
    std::vector<uint8_t> stable(size);
    size_t nUn = 0;
    for (size_t i = 0; i < size; ++i) {
        stable[i] = A[i] == B[i];
        if (!stable[i]) ++nUn;
    }
    lg("idle-stability: %zu of %zu bytes stable, %zu unstable (counters/timers/timestamps)", size - nUn, size, nUn);
    {
        std::string s;
        size_t shown = 0;
        for (size_t i = 0; i < size && shown < 24; ++i)
            if (!stable[i]) {
                char t[24];
                std::snprintf(t, sizeof t, "%zX ", i);
                s += t;
                ++shown;
            }
        lg("  first unstable offsets: %s", s.c_str());
    }

    struct Hit {
        size_t off;
        uint8_t mask;
        bool reverts;
    };
    static const char* pcmd[3] = {"km.left", "km.right", "km.middle"};
    static const char* pname[3] = {"LMB", "RMB", "MMB"};
    std::vector<Hit> hits[3];
    for (int p = 0; p < 3; ++p) {
        char down[32], up[32];
        std::snprintf(down, sizeof down, "%s(1)\r\n", pcmd[p]);
        std::snprintf(up, sizeof up, "%s(0)\r\n", pcmd[p]);
        gBox.send(down);
        Sleep(DWELL);
        std::vector<uint8_t> P;
        if (!readImg(ctx, base, size, P)) { lg("read P(%s) failed", pname[p]); continue; }
        gBox.send(up);
        Sleep(DWELL * 2);
        std::vector<uint8_t> R;
        if (!readImg(ctx, base, size, R)) { lg("read R(%s) failed", pname[p]); continue; }
        for (size_t i = 0; i < size; ++i) {
            if (!stable[i]) continue;  // counters are excluded by definition
            const uint8_t d = (uint8_t)(B[i] ^ P[i]);
            if (d) hits[p].push_back({i, d, R[i] == B[i]});
        }
        std::string s;
        size_t rev = 0;
        for (size_t k = 0; k < hits[p].size(); ++k) {
            if (hits[p][k].reverts) ++rev;
            if (k < 16) {
                char t[40];
                std::snprintf(t, sizeof t, "%zX:%02X>%02X%s ", hits[p][k].off, B[hits[p][k].off], (uint8_t)(B[hits[p][k].off] ^ hits[p][k].mask),
                              hits[p][k].reverts ? "(rev)" : "");
                s += t;
            }
        }
        lg("%s: %zu stable bytes changed, %zu revert -> %s", pname[p], hits[p].size(), rev, s.c_str());
    }

    bool any = false;
    for (const Layout& L : LAY) {
        const int off[3] = {L.o1, L.o2, L.o4};
        const int bts[3] = {L.b1, L.b2, L.b4};
        for (const Hit& h1 : hits[0]) {
            if (!(h1.mask & (1u << bts[0]))) continue;
            const int64_t delta = (int64_t)h1.off - off[0];
            bool ok = true;
            for (int p = 1; p < 3 && ok; ++p) {
                bool found = false;
                for (const Hit& h : hits[p])
                    if ((int64_t)h.off - off[p] == delta && (h.mask & (1u << bts[p]))) { found = true; break; }
                if (!found) ok = false;
            }
            if (ok) {
                lg("MATCH layout=%s arrayBase=0x%llX ctx=0x%lX %s | %s", L.name, (unsigned long long)(base + delta), (unsigned long)ctx, module, L.map);
                any = true;
            }
        }
    }
    if (!any) lg("no layout match from LMB/RMB/MMB on stable bytes");

    ULONG64 rva = 0;
    std::string mod;
    if (pdbRva(ctx, base, "gafAsyncKeyState", rva, mod)) {
        Samples s;
        if (grab(ctx, base + rva, 192, s)) {
            int n = 0;
            for (size_t i = 0; i < 192; ++i)
                if (s.idle[i] != s.lmb[i] || s.idle[i] != s.rmbb[i] || s.idle[i] != s.mmbb[i]) {
                    lg("  pdb-window off=0x%zX idle=%02X lmb=%02X rmb=%02X mmb=%02X", i, s.idle[i], s.lmb[i], s.rmbb[i], s.mmbb[i]);
                    ++n;
                }
            lg("pdb window (base+rva, 192 bytes): %d bytes react to the pulses", n);
        }
    }
}

int main(int argc, char** argv) {
    std::string mode = "probe", port = "COM3", dllDir = exeDir(), module = "win32kbase.sys";
    DWORD onlyCtx = (DWORD)-1;
    int rounds = 3, key1 = 0x45, key2 = 0x57;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "probe" || a == "scan" || a == "scanall" || a == "env" || a == "ekey" || a == "find") mode = a;
        else if (a == "--port" && i + 1 < argc) port = argv[++i];
        else if (a == "--dll" && i + 1 < argc) dllDir = argv[++i];
        else if (a == "--module" && i + 1 < argc) module = argv[++i];
        else if (a == "--ctx" && i + 1 < argc) onlyCtx = (DWORD)strtoul(argv[++i], nullptr, 0);
        else if (a == "--rounds" && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (a == "--key1" && i + 1 < argc) key1 = (int)strtol(argv[++i], nullptr, 0);
        else if (a == "--key2" && i + 1 < argc) key2 = (int)strtol(argv[++i], nullptr, 0);
    }
    std::string logPath = exeDir() + "\\keyprobe.log";
    gLog = _fsopen(logPath.c_str(), "w", _SH_DENYNO);
    lg("keyprobe mode=%s port=%s dll=%s module=%s", mode.c_str(), port.c_str(), dllDir.c_str(), module.c_str());

    if (!loadVmm(dllDir)) return 2;
    LPCSTR av[] = {"", "-device", "fpga", "-waitinitialize"};
    for (int attempt = 0; attempt < 6 && !gVmm; ++attempt) {
        if (attempt) {
            lg("[..] VMM init retry %d ...", attempt);
            Sleep(1500);
        }
        gVmm = VMMDLL_Initialize(4, av);
    }
    if (!gVmm) { lg("[!!] VMMDLL_Initialize failed after retries (card busy / needs power cycle?)"); return 3; }
    lg("[ok] VMM initialized");

    if (mode != "ekey") {  // ekey = strictly read-only, never touch the box
        if (!gBox.open(port.c_str())) lg("[!!] box not opened - pulse tests will be meaningless");
        else {
            gBox.send("km.version()\r\n");
            Sleep(120);
            std::string r = gBox.drain();
            std::string ident;
            for (char ch : r)
                if (ch >= 32 && ch < 127) ident += ch;
            lg("[box] reply: %s", ident.c_str());
        }
    } else {
        lg("[ekey] box untouched (no km.* commands)");
    }

    std::vector<Proc> procs = enumProcs();
    lg("processes: %zu", procs.size());

    if (mode == "scanall") {
        // ordered, de-duplicated scan contexts: system first, then session-1 host processes
        std::vector<DWORD> ctxs;
        auto add = [&](DWORD x) {
            if (x && std::find(ctxs.begin(), ctxs.end(), x) == ctxs.end()) ctxs.push_back(x);
        };
        add(4);
        add(4 | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY);
        const char* order[] = {"csrss.exe", "winlogon.exe", "DayZ_x64.exe"};
        for (const char* nm : order)
            for (const auto& p : procs)
                if (p.name == nm) {
                    add(p.pid);
                    add(p.pid | VMMDLL_PID_PROCESS_WITH_KERNELMEMORY);
                }
        const char* ms[] = {"win32kbase.sys", "win32kfull.sys", "win32k.sys"};
        for (const char* m : ms) {
            for (DWORD c : ctxs) {
                if (scanOne(c, m)) {
                    lg(">>> %s : confirmed in ctx 0x%lX - stop scanning this module", m, (unsigned long)c);
                    break;
                }
            }
        }
    } else if (mode == "scan") {
        if (onlyCtx == (DWORD)-1)
            scanOne(4, module.c_str());
        else
            scanOne(onlyCtx, module.c_str());
    } else if (mode == "find") {
        doFind(onlyCtx == (DWORD)-1 ? 4 : onlyCtx, module.c_str());
    } else if (mode == "env") {
        doProbe(procs, false);
    } else if (mode == "ekey") {
        doEKey(onlyCtx == (DWORD)-1 ? 4 : onlyCtx, module.c_str(), key1, key2, rounds);
    } else {
        doProbe(procs, true);
    }

    lg("done.");
    gBox.close();
    if (gVmm) VMMDLL_Close(gVmm);
    if (gLog) std::fclose(gLog);
    return 0;
}




