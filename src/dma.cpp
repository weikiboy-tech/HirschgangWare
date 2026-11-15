#include "process.h"
#include "clock.h"
#include "telemetry.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma warning(push)
#pragma warning(disable : 4200 4201 4324)
#include <vmmdll.h>
#pragma warning(pop)

namespace {

void (*gKeyLog)(const char*) = nullptr;

using PfnInitialize = VMM_HANDLE (*)(DWORD, LPCSTR*);
using PfnClose = VOID (*)(VMM_HANDLE);
using PfnMemFree = VOID (*)(PVOID);
using PfnPidGetFromName = BOOL (*)(VMM_HANDLE, LPCSTR, PDWORD);
using PfnMemReadEx = BOOL (*)(VMM_HANDLE, DWORD, ULONG64, PBYTE, DWORD, PDWORD, ULONG64);
using PfnGetModuleFromNameU = BOOL (*)(VMM_HANDLE, DWORD, LPCSTR, PVMMDLL_MAP_MODULEENTRY*, DWORD);
using PfnGetVadU = BOOL (*)(VMM_HANDLE, DWORD, BOOL, PVMMDLL_MAP_VAD*);
using PfnGetModuleU = BOOL (*)(VMM_HANDLE, DWORD, PVMMDLL_MAP_MODULE*, DWORD);
using PfnPdbLoad = BOOL (*)(VMM_HANDLE, DWORD, ULONG64, LPSTR);
using PfnPdbSymbolAddress = BOOL (*)(VMM_HANDLE, LPCSTR, LPCSTR, PQWORD);
using PfnScatterInitialize = VMMDLL_SCATTER_HANDLE (*)(VMM_HANDLE, DWORD, DWORD);
using PfnScatterPrepareEx = BOOL (*)(VMMDLL_SCATTER_HANDLE, QWORD, DWORD, PBYTE, PDWORD);
using PfnScatterExecuteRead = BOOL (*)(VMMDLL_SCATTER_HANDLE);
using PfnScatterCloseHandle = VOID (*)(VMMDLL_SCATTER_HANDLE);
using PfnConfigSet = BOOL (*)(VMM_HANDLE, ULONG64, ULONG64);
using PfnMemReadScatter = DWORD (*)(VMM_HANDLE, DWORD, PPMEM_SCATTER, DWORD, DWORD);
using PfnVirt2Phys = BOOL (*)(VMM_HANDLE, DWORD, ULONG64, PULONG64);
using PfnGetPhysMem = BOOL (*)(VMM_HANDLE, PVMMDLL_MAP_PHYSMEM*);
using PfnGetEatU = BOOL (*)(VMM_HANDLE, DWORD, LPCSTR, PVMMDLL_MAP_EAT*);
using PfnPidList = BOOL (*)(VMM_HANDLE, PDWORD, PSIZE_T);
using PfnProcessGetModuleBaseU = ULONG64 (*)(VMM_HANDLE, DWORD, LPCSTR);

constexpr uint64_t kPageSize = 0x1000;
constexpr uint64_t kPageMask = ~uint64_t(0xFFF);
// window of the blind MZ scan used to look for further win32kbase image copies
constexpr uint64_t kWin32kScanLo = 0xFFFFF80000000000ull;
constexpr uint64_t kWin32kScanHi = 0xFFFFF80100000000ull;
constexpr DWORD kPidPhysical = (DWORD)-1;
constexpr size_t kChunkPages = 48;
constexpr size_t kChunkPagesBg = 16;

enum : int { kPrioBg = 0, kPrioNormal = 1, kPrioUrgent = 2 };
thread_local int tlPrio = kPrioNormal;
thread_local bool tlUrgent = false;

std::string exeDirectory() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p = buf;
    return p.substr(0, p.find_last_of("\\/"));
}

bool fileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

DWORD toPageProtect(DWORD vadProt) {
    switch (vadProt & 7) {
        case 1: return PAGE_READONLY;
        case 2: return PAGE_EXECUTE;
        case 3: return PAGE_EXECUTE_READ;
        case 4: return PAGE_READWRITE;
        case 5: return PAGE_WRITECOPY;
        case 6: return PAGE_EXECUTE_READWRITE;
        case 7: return PAGE_EXECUTE_WRITECOPY;
        default: return PAGE_NOACCESS;
    }
}

thread_local int tlHeld = 0;
struct TimedLock {
    std::recursive_mutex& m;
#ifndef ICKZA_NO_DIAG
    uint64_t waitUs = 0;
#endif
    explicit TimedLock(std::recursive_mutex& mx) : m(mx) {
#ifndef ICKZA_NO_DIAG
        if (!m.try_lock()) {
            const uint64_t t0 = qpcUs();
            m.lock();
            waitUs = qpcUs() - t0;
        }
#else
        m.lock();
#endif
        ++tlHeld;
    }
    ~TimedLock() {
        --tlHeld;
        m.unlock();
    }
};

struct UrgentMark {
    std::atomic<int>& n;
    bool on;
    explicit UrgentMark(std::atomic<int>* c) : n(c[tlPrio]), on(tlPrio > kPrioBg) {
        if (on) n.fetch_add(1);
    }
    void done() {
        if (on) n.fetch_sub(1);
        on = false;
    }
    ~UrgentMark() { done(); }
};

class DmaProcess : public Process {
public:
    DmaProcess(std::string device, std::string dllDir, bool ownRefresh)
        : device_(std::move(device)), dllDir_(std::move(dllDir)), ownRefresh_(ownRefresh), noPaging_(ownRefresh) {}

    void maintain() override {
        TimedLock lk(mx_);
        const ULONGLONG now = GetTickCount64();
        if (physRead_ && now - lastPteFlush_ >= 1000) {
            lastPteFlush_ = now;
            pteCache_.clear();
            tel::count(tel::C_PTE_FLUSH);
        }
        if (!ownRefresh_ || !configSet_ || !vmm_ || now - lastTlb_ < 1000) return;
        lastTlb_ = now;
        pteCache_.clear();
        const uint64_t t0 = qpcUs();
        configSet_(vmm_, VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL, 1);
        tel::time(tel::T_REFRESH, qpcUs() - t0);
        tel::count(tel::C_REFRESHES);
    }

    ~DmaProcess() override {
        if (vmm_ && close_) close_(vmm_);
        if (dll_) FreeLibrary(dll_);
    }

    bool attach(const std::string& exeName, std::string& err) override {
        if (!vmm_ && !init(err)) return false;

        DWORD pid = 0;
        if (!pidFromName_(vmm_, exeName.c_str(), &pid)) {
            const ULONGLONG now = GetTickCount64();
            bool found = false;
            if (ownRefresh_ && configSet_ && now - lastProcRefresh_ >= 2000) {
                lastProcRefresh_ = now;
                configSet_(vmm_, VMMDLL_OPT_REFRESH_FREQ_MEDIUM, 1);
                found = pidFromName_(vmm_, exeName.c_str(), &pid) != FALSE;
            }
            if (!found) {
                err = exeName + " not running";
                return false;
            }
        }
        PVMMDLL_MAP_MODULEENTRY entry = nullptr;
        if (!moduleFromName_(vmm_, pid, exeName.c_str(), &entry, VMMDLL_MODULE_FLAG_NORMAL) || !entry) {
            err = "main module not found yet";
            return false;
        }
        mod_.name = entry->uszText ? entry->uszText : exeName;
        mod_.base = static_cast<uintptr_t>(entry->vaBase);
        mod_.size = entry->cbImageSize;
        memFree_(entry);
        pid_ = pid;
        pteCache_.clear();
        captureAliveProbes();
        return true;
    }

    bool read(uintptr_t addr, void* out, size_t size) const override {
        const uint64_t t0 = qpcUs();
        yieldToHigher();
        UrgentMark um(waiting_);
        TimedLock lk(mx_);
        um.done();
        if (!vmm_ || !pid_) return false;
        bool ok;
        if (physRead_) {
            ReadReq r{addr, out, size, false};
            readPhys(&r, 1);
            ok = r.ok;
        } else {
            ok = readVirt(addr, out, size);
            tel::count(tel::C_SCATTERS);
        }
#ifndef ICKZA_NO_DIAG
        tel::CallInfo ci;
        ci.single = true;
        ci.reqs = 1;
        ci.reqsOk = ok ? 1 : 0;
        ci.bytes = size;
        ci.bytesOk = ok ? size : 0;
        ci.totalUs = qpcUs() - t0;
        ci.lockUs = lk.waitUs;
        tel::call(ci);
#endif
        return ok;
    }

    size_t readMany(std::vector<ReadReq>& reqs) const override {
        const uint64_t t0 = qpcUs();
        if (reqs.empty()) return 0;
        size_t n = 0;
#ifndef ICKZA_NO_DIAG
        uint64_t lockUs = 0;
#endif
        std::vector<uint64_t> pages;
        std::vector<ReadReq*> deferred;
        for (size_t begin = 0; begin < reqs.size();) {
            size_t end = reqs.size();
            if (!tlUrgent) {
                pages.clear();
                for (end = begin; end < reqs.size(); ++end) {
                    const ReadReq& r = reqs[end];
                    const uint64_t first = uint64_t(r.addr) & kPageMask;
                    const uint64_t last = r.size ? (uint64_t(r.addr) + r.size - 1) & kPageMask : first;
                    size_t added = 0;
                    for (uint64_t va = first; va <= last; va += kPageSize)
                        if (std::find(pages.begin(), pages.end(), va) == pages.end()) ++added;
                    if (end > begin && pages.size() + added > (tlPrio == kPrioBg ? kChunkPagesBg : kChunkPages)) break;
                    for (uint64_t va = first; va <= last; va += kPageSize)
                        if (std::find(pages.begin(), pages.end(), va) == pages.end()) pages.push_back(va);
                }
            }
            yieldToHigher();
            UrgentMark um(waiting_);
            TimedLock lk(mx_);
            um.done();
#ifndef ICKZA_NO_DIAG
            lockUs += lk.waitUs;
#endif
            if (!vmm_ || !pid_) return n;
            n += readManyLocked(reqs.data() + begin, end - begin, &deferred);
            begin = end;
        }
        for (size_t begin = 0; begin < deferred.size();) {
            const size_t end = tlUrgent ? deferred.size() : std::min(deferred.size(), begin + (tlPrio == kPrioBg ? kChunkPagesBg : kChunkPages));
            yieldToHigher();
            UrgentMark um(waiting_);
            TimedLock lk(mx_);
            um.done();
#ifndef ICKZA_NO_DIAG
            lockUs += lk.waitUs;
#endif
            if (!vmm_ || !pid_) return n;
            n += readVirtBatchLocked(deferred.data() + begin, end - begin);
            begin = end;
        }
#ifndef ICKZA_NO_DIAG
        tel::CallInfo ci;
        ci.reqs = static_cast<uint32_t>(reqs.size());
        for (const ReadReq& r : reqs) {
            ci.bytes += r.size;
            if (r.ok) {
                ++ci.reqsOk;
                ci.bytesOk += r.size;
            }
        }
        ci.totalUs = qpcUs() - t0;
        ci.lockUs = lockUs;
        tel::call(ci);
#endif
        return n;
    }

    bool readable(uintptr_t addr) const override {
        uint8_t b;
        return read(addr, &b, 1);
    }

    std::vector<Region> regions(uint64_t maxAddr) const override {
        TimedLock lk(mx_);
        std::vector<Region> out;
        if (!vmm_ || !pid_) return out;
        PVMMDLL_MAP_VAD vad = nullptr;
        if (!vadU_(vmm_, pid_, FALSE, &vad) || !vad) return out;
        for (DWORD i = 0; i < vad->cMap; ++i) {
            const VMMDLL_MAP_VADENTRY& e = vad->pMap[i];
            if (e.vaStart >= maxAddr || e.vaEnd < e.vaStart) continue;
            Region r;
            r.base = static_cast<uintptr_t>(e.vaStart);
            r.size = static_cast<size_t>(e.vaEnd - e.vaStart + 1);
            r.protect = toPageProtect(e.Protection);
            if (e.Protection & 0x10) r.protect |= PAGE_GUARD;
            r.type = e.fImage ? MEM_IMAGE : e.fPrivateMemory ? MEM_PRIVATE : MEM_MAPPED;
            if (r.protect & PAGE_GUARD || r.protect == PAGE_NOACCESS) continue;
            out.push_back(r);
        }
        memFree_(vad);
        return out;
    }

    bool alive() const override {
        TimedLock lk(mx_);
        if (!vmm_ || !pid_ || !mod_.base) return false;
        struct PagingOk {
            bool& f;
            explicit PagingOk(bool& b) : f(b) { f = true; }
            ~PagingOk() { f = false; }
        } po(pagingOk_);
        uint16_t mz = 0;
        if (read(mod_.base, &mz, sizeof mz) && mz == 0x5A4D) return true;
        for (const auto& p : probes_) {
            uint8_t b[sizeof p.bytes];
            if (read(p.addr, b, sizeof b) && memcmp(b, p.bytes, sizeof b) == 0) return true;
        }
        return false;
    }

private:
    struct AliveProbe {
        uintptr_t addr;
        uint8_t bytes[16];
    };

    void captureAliveProbes() {
        constexpr int kProbes = 16;
        probes_.clear();
        uint8_t hdr[0x1000];
        if (!read(mod_.base, hdr, sizeof hdr)) return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hdr);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > sizeof hdr) return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(hdr + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        const int nsec = nt->FileHeader.NumberOfSections;
        if (reinterpret_cast<const uint8_t*>(sec + nsec) > hdr + sizeof hdr) return;
        std::vector<std::pair<uintptr_t, size_t>> ranges;
        size_t total = 0;
        for (int i = 0; i < nsec; ++i) {
            const DWORD c = sec[i].Characteristics;
            if (c & IMAGE_SCN_MEM_WRITE) continue;
            if (!(c & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_INITIALIZED_DATA))) continue;
            const size_t len = std::min<size_t>(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
            if (len < 0x1000) continue;
            ranges.push_back({mod_.base + sec[i].VirtualAddress, len});
            total += len;
        }
        if (!total) return;
        const size_t step = total / (kProbes * 2);
        for (int k = 0; k < kProbes * 2 && int(probes_.size()) < kProbes; ++k) {
            size_t off = step * size_t(k) + 0x800;
            for (const auto& r : ranges) {
                if (off < r.second) {
                    AliveProbe p;
                    p.addr = r.first + off;
                    if (off + 16 <= r.second && read(p.addr, p.bytes, sizeof p.bytes)) probes_.push_back(p);
                    break;
                }
                off -= r.second;
            }
        }
    }

    DWORD virtFlags() const { return VMMDLL_FLAG_NOCACHE | (noPaging_ && !pagingOk_ ? VMMDLL_FLAG_NOPAGING : 0); }

    // read a VA in the address space of an arbitrary process (the session-1 ctx that owns win32k data)
    bool readAt(DWORD ctxPid, uint64_t addr, void* out, size_t size) const {
        DWORD got = 0;
        return readEx_(vmm_, ctxPid, addr, static_cast<PBYTE>(out), static_cast<DWORD>(size), &got, virtFlags()) && got == size;
    }

    // gafAsyncKeyState layout is 2 bits per VK (low = down, high = toggle) in a 64 byte block OR
    // 1 bit per VK in a 32 byte block, depending on build. Decode to 1 byte/vk with 0x80 for callers.
    static void decodeKeyBitmap(const uint8_t in[64], uint8_t out[256], bool oneBit) {
        if (oneBit) {
            for (int vk = 0; vk < 256; ++vk) out[vk] = (in[vk >> 3] & (1 << (vk & 7))) ? 0x80 : 0x00;
        } else {
            for (int vk = 0; vk < 256; ++vk) out[vk] = (in[(vk * 2) / 8] & (1 << ((vk % 4) * 2))) ? 0x80 : 0x00;
        }
    }

    mutable bool keyLayout1_ = false;
    mutable int refineTries_ = 0;
    mutable int keyFailN_ = 0;  // consecutive failed key reads -> re-verify after 3
    mutable int keyScanRounds_ = 0;  // cheap scan rounds without a hit -> then one full-image refine pass
    mutable uint64_t lastRefAddr_ = 0;
    mutable bool lastRefLayout1_ = false;

    struct KeyCand {
        DWORD pid;
        uint64_t addr;
        const char* how;
        uint32_t size = 0;
    };

    // find win32kbase PE copies (session copies are mapped in session space, not in any module list)
    // fast: 2-byte MZ probe over 4 GB in 64 KB steps, PE + size check only on hits
    void scanWin32kbase(DWORD ctx, std::vector<std::pair<uint64_t, uint32_t>>& out) const {
        const uint64_t lo = kWin32kScanLo, hi = kWin32kScanHi;
        for (uint64_t a = lo; a < hi; a += 0x10000) {
            uint16_t mz = 0;
            DWORD g = 0;
            if (!readEx_(vmm_, ctx, a, reinterpret_cast<PBYTE>(&mz), 2, &g, virtFlags()) || g != 2) continue;
            if (mz != 0x5A4D) continue;  // 'MZ'
            uint8_t b[0x60] = {};
            DWORD g2 = 0;
            if (!readEx_(vmm_, ctx, a, b, sizeof b, &g2, virtFlags()) || g2 != sizeof b) continue;
            uint32_t pe = 0;
            std::memcpy(&pe, b + 0x3C, 4);
            if (pe < 0x40 || pe > 0x400) continue;
            uint8_t ph[0x60] = {};
            DWORD g3 = 0;
            if (!readEx_(vmm_, ctx, a + pe, ph, sizeof ph, &g3, virtFlags()) || g3 != sizeof ph) continue;
            if (ph[0] != 'P' || ph[1] != 'E') continue;
            uint32_t sz = 0;
            std::memcpy(&sz, ph + 0x50, 4);
            if (sz < 0x200000 || sz > 0x800000) continue;  // win32kbase/win32kfull sized images
            out.push_back({a, sz});
        }
    }

    // ---- refine: search a whole win32kbase image for the offset where OUR keys flip ----
    mutable std::vector<std::pair<DWORD, uint64_t>> refBase_;
    mutable std::vector<uint32_t> refSize_;
    mutable std::vector<std::vector<uint8_t>> refA_;
    mutable std::vector<std::vector<uint32_t>> refUn_;  // prefix sum of idle-unstable bytes (counters)

    bool readImage(DWORD ctx, uint64_t base, uint32_t size, std::vector<uint8_t>& img) const {
        img.assign(size, 0);
        const DWORD fl = VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_ZEROPAD_ON_FAIL;
        // A 4k read costs one device round trip; win32kbase is 2-8 MB, so the old 4k loop
        // burned thousands of round trips per image (and the refine repeats that every 5 s).
        // Batch 64k and fall back to 4k pages only if a batched read came back short.
        for (size_t o = 0; o < img.size(); o += 0x10000) {
            const DWORD n = DWORD(std::min<size_t>(0x10000, img.size() - o));
            DWORD g = 0;
            readEx_(vmm_, ctx, base + o, img.data() + o, n, &g, fl);
            if (g != n) {
                for (size_t p = 0; p < size_t(n); p += 0x1000) {
                    const DWORD n2 = DWORD(std::min<size_t>(0x1000, size_t(n) - p));
                    DWORD g2 = 0;
                    readEx_(vmm_, ctx, base + o + p, img.data() + o + p, n2, &g2, fl);
                }
            }
        }
        return true;
    }

    void refineArm() const {
        const uint64_t armT0 = qpcUs();

        refBase_.clear();
        refSize_.clear();
        refA_.clear();
        refUn_.clear();
        int seen = 0;
        auto addRef = [&](const KeyCand& k) {
            const uint64_t base = k.addr - keyRva_;
            std::vector<uint8_t> img, img2;
            readImage(k.pid, base, k.size, img);
            readImage(k.pid, base, k.size, img2);  // 2nd idle read: counters/timers are unstable, a bitmap is not
            std::vector<uint32_t> un(img.size() + 1, 0);
            size_t nUn = 0;
            for (size_t i = 0; i < img.size(); ++i) {
                const bool dif = img[i] != img2[i];
                if (dif) ++nUn;
                un[i + 1] = un[i] + (dif ? 1u : 0u);
            }
            int nz = 0;
            for (size_t i = 0; i < img2.size(); i += 0x100) nz += img2[i] != 0;
            if (gKeyLog) {
                char s[200];
                std::snprintf(s, sizeof s, "[trg] refine base=0x%llX size=0x%X nz=%d unstable=%zu (idle gate)", (unsigned long long)base, k.size, nz,
                              nUn);
                gKeyLog(s);
            }
            refBase_.push_back({k.pid, base});
            refSize_.push_back(k.size);
            refA_.push_back(std::move(img2));
            refUn_.push_back(std::move(un));
        };
        const uint64_t pref = keyState_;
        for (const auto& k : keyCands2_)  // preferred: image of the tentative candidate
            if (k.size && k.addr == pref) {
                addRef(k);
                break;
            }
        for (const auto& k : keyCands2_) {
            if (refBase_.size() >= 6) break;
            if (!k.size) continue;  // needs a known image size (set for pdb/mod/scan candidates)
            ++seen;
            if (k.addr == pref) continue;
            addRef(k);
        }
        if (gKeyLog) {
            char s[176];
            std::snprintf(s, sizeof s, "[trg] refine armed %zu images (of %d cands) in %.0f ms - HOLD the keys", refBase_.size(), seen,
                          (qpcUs() - armT0) / 1000.0);
            gKeyLog(s);
        }
    }

    bool refineCompare() const {
        static const uint8_t vks[] = {0x10, 0x20, 0x41, 0x44, 0x45, 0x53, 0x57};
        int bestScore = -1, bestDiff = 0, bestDown = 0;
        uint64_t bestAddr = 0;
        DWORD bestCtx = 0;
        bool bestL1 = false;
        uint64_t bestOff = 0;
        std::vector<uint8_t> df;
        for (size_t ii = 0; ii < refA_.size(); ++ii) {
            std::vector<uint8_t> b;
            readImage(refBase_[ii].first, refBase_[ii].second, refSize_[ii], b);
            const std::vector<uint8_t>& a = refA_[ii];
            const size_t n = a.size();
            df.assign(n, 0);
            for (size_t i = 0; i < n; ++i) df[i] = a[i] != b[i] ? 1 : 0;
            // rolling count of changed bytes in the 22-byte VK window -> skip untouched offsets fast
            int win = 0;
            const size_t wend = n < 22 ? n : 22;
            for (size_t i = 0; i < wend; ++i) win += df[i];
            for (size_t off = 0; off + 64 <= n; ++off) {
                // idle-stability gate: a window containing counter/timer bytes is not the key bitmap
                const bool gate = ii >= refUn_.size() || refUn_[ii][off + 64] == refUn_[ii][off];
                if (gate && win >= 1) {
                    // model: at sample A (idle) the 7 bits are 0, at sample B (pressed) they are 1
                    int heldB = 0, rise = 0;
                    for (const uint8_t k : vks) {
                        const int bi2 = (k * 2) / 8, bo2 = (k % 4) * 2;
                        const int a2 = (a[off + bi2] >> bo2) & 1, b2 = (b[off + bi2] >> bo2) & 1;
                        if (b2) ++heldB;
                        if (!a2 && b2) ++rise;
                    }
                        const int sc = (heldB == rise ? 30 : 0) + rise * 10 - (heldB - rise) * 5;
                    if (sc > bestScore) {
                        bestScore = sc;
                        bestAddr = refBase_[ii].second + off;
                        bestCtx = refBase_[ii].first;
                        bestL1 = false;
                        bestOff = off;
                        bestDiff = rise;
                        bestDown = heldB;
                    }
                }
                if (off + 22 < n) win += df[off + 22];
                win -= df[off];
            }
        }
        if (gKeyLog) {
            char s[220];
            std::snprintf(s, sizeof s, "[trg] refine BEST rise=%d heldB=%d layout2 ctx=0x%lX va=0x%llX off=0x%llX", bestDiff, bestDown,
                          (unsigned long)bestCtx, (unsigned long long)bestAddr, (unsigned long long)bestOff);
            gKeyLog(s);
        }
        // exact match: every currently-down test key also went down between the two samples
        const bool strong = bestDiff >= 1 && bestDown == bestDiff;
        if (!strong) return false;
        if (bestDiff >= 2 || (bestAddr == lastRefAddr_ && bestL1 == lastRefLayout1_)) {
            keyCtxPid_ = bestCtx;
            keyState_ = bestAddr;
            keyLayout1_ = bestL1;
            keyHow_ = "refined";
            keyVerified_ = true;
            keyDiag_ = false;
            keyPrevValid_ = false;
            keyInfo_ = "refined (confirmed differential search)";
            saveKeyArray();
            return true;
        }
        lastRefAddr_ = bestAddr;
        lastRefLayout1_ = bestL1;
        if (gKeyLog) gKeyLog("[trg] refine candidate stored (needs one more hit to confirm)");
        return false;
    }


    static std::string keyArrayPath() { return exeDirectory() + "\\keyarray.txt"; }

    bool loadSavedKeyArray() const {
        FILE* f = nullptr;
        if (fopen_s(&f, keyArrayPath().c_str(), "r") || !f) return false;
        unsigned long ctx = 0;
        unsigned long long va = 0;
        int lay = 0;
        const int n = std::fscanf(f, "%lu %llu %d", &ctx, &va, &lay);
        std::fclose(f);
        if (n != 3 || !ctx || !va) return false;
        uint8_t raw[64] = {};
        if (!readAt(ctx, va, raw, sizeof raw)) return false;
        int nz = 0;
        for (const uint8_t b : raw) nz += b != 0;
        keyCtxPid_ = ctx;
        keyState_ = va;
        keyLayout1_ = lay != 0;
        keyHow_ = "saved";
        return nz > 0;
    }

    void saveKeyArray() const {
        FILE* f = nullptr;
        if (fopen_s(&f, keyArrayPath().c_str(), "w") || !f) return;
        std::fprintf(f, "%lu %llu %d\n", (unsigned long)keyCtxPid_, (unsigned long long)keyState_, keyLayout1_ ? 1 : 0);
        std::fclose(f);
    }

    // fallback path: csrss.exe -> win32ksgd.sys session slots (+0x3110) -> ptr -> ptr -> +0x3690
    bool resolveViaCsrss(DWORD csrssPid, uint64_t& addr, DWORD& ctx) const {
        if (!procModBase_) return false;
        const DWORD ctxs[2] = {csrssPid | 0x80000000u, csrssPid};
        for (const DWORD c : ctxs) {
            const uint64_t base = procModBase_(vmm_, c, "win32ksgd.sys");
            if (!base) continue;
            uint64_t v1 = 0, v2 = 0, st = 0;
            if (!readAt(c, base + 0x3110, &v1, 8) || v1 < 0xFFFF000000000000ull) continue;
            if (!readAt(c, v1, &v2, 8) || v2 < 0xFFFF000000000000ull) continue;
            if (!readAt(c, v2, &st, 8) || st < 0xFFFF000000000000ull) continue;
            addr = st + 0x3690;
            ctx = c;
            return true;
        }
        return false;
    }

    // target keyboard state: win32kbase!gafAsyncKeyState (256 bytes, high bit = down)
    // ---- DMA keyboard path. Set to false to get the exact behaviour of the "all mouse buttons via box" build:
    //      no scanning, no image reads, no lag. Trigger/bind then use box + this PC only.
    //      When on: resolve once (result is kept in keyarray.txt), verification runs at most 6x5s,
    //      then the path is passive (one 64 byte read per caller tick) - see docs/INPUT_RESEARCH.md.
    static constexpr bool keyEnable_ = true;

    bool keyState(uint8_t out[256]) const override {
        if (!keyEnable_) {
            if (!keyTried_) {
                keyTried_ = true;
                keyInfo_ = "off (box + this PC only)";
            }
            return false;
        }
        if (!keyTried_) resolveKey();
        if (!keyState_) return false;
        uint8_t raw[64] = {};
        if (!readAt(keyCtxPid_, keyState_, raw, sizeof raw)) {
            if (++keyFailN_ == 1 && gKeyLog) gKeyLog("[trg] keydiag read FAILED");
            if (keyFailN_ >= 3 && keyPhase_ == 5) {  // verified state gone (reboot / session moved) -> look again
                keyFailN_ = 0;
                keyPhase_ = 1;
                if (gKeyLog) gKeyLog("[trg] keydiag read failed 3x - re-verifying the key array");
            }
            return false;
        }
        keyFailN_ = 0;
        decodeKeyBitmap(raw, out, keyLayout1_);
        diagKey(out, raw);
        if (keyPhase_ == 1 || keyPhase_ == 4) findKey();  // armed: the compare pass runs automatically 5s after arming
        return true;
    }

    mutable bool keyPhysLogged_ = false;
    mutable const char* keyHow_ = "-";
    mutable DWORD keyCtxPid_ = 0;
    mutable std::vector<KeyCand> keyCands2_;

    mutable uint8_t keyPrev_[256] = {};
    mutable bool keyPrevValid_ = false;
    mutable bool keyDiag_ = false;
    mutable ULONGLONG keyLogAt_ = 0;

    void diagKey(const uint8_t* b, const uint8_t* raw) const {
        if (!gKeyLog) return;
        const ULONGLONG now = GetTickCount64();
        char s[420];
        if (!keyDiag_) {
            keyDiag_ = true;
            int rnz = 0;
            char hex[3 * 64 + 1] = {};
            size_t at = 0;
            for (int i = 0; i < 64; ++i) {
                rnz += raw[i] != 0;
                at += size_t(std::snprintf(hex + at, sizeof hex - at, "%02X", raw[i]));
            }
            std::snprintf(s, sizeof s, "[trg] keydiag ctxpid=%lu how=%s va=0x%llX rawnz=%d raw=%s", (unsigned long)keyCtxPid_, keyHow_,
                          (unsigned long long)keyState_, rnz, hex);
            gKeyLog(s);
        }
        if (keyPrevValid_) {
            char line[200] = {};
            size_t at = 0;
            int n = 0;
            for (int i = 0; i < 256 && n < 12; ++i)
                if (b[i] != keyPrev_[i]) {
                    at += size_t(std::snprintf(line + at, sizeof line - at, " %02X:%02X>%02X", i, keyPrev_[i], b[i]));
                    ++n;
                }
            if (n) {
                char hex[129] = {};
                for (int i = 0; i < 64; ++i) std::snprintf(hex + i * 2, 3, "%02X", raw[i]);
                char s2[560];
                std::snprintf(s2, sizeof s2, "[trg] keydiag change%s raw=%s", line, hex);
                gKeyLog(s2);
            }
        }
        std::memcpy(keyPrev_, b, 256);
        keyPrevValid_ = true;
        if (now - keyLogAt_ >= 1500) {
            keyLogAt_ = now;
            char line[200] = {};
            size_t at = 0;
            int n = 0, nz = 0;
            for (int i = 0; i < 256; ++i)
                if (b[i]) {
                    ++nz;
                    if (n < 12) at += size_t(std::snprintf(line + at, sizeof line - at, " %02X", i));
                    ++n;
                }
            std::snprintf(s, sizeof s, "[trg] keydiag down=%d%s", nz, line);
            gKeyLog(s);
            static const uint8_t ck[] = {0x10, 0x20, 0x41, 0x44, 0x45, 0x53, 0x57};
            char c[140] = {};
            size_t ca = 0;
            for (const uint8_t k : ck) ca += size_t(std::snprintf(c + ca, sizeof c - ca, " %02X=%d", k, b[k] ? 1 : 0));
            std::snprintf(s, sizeof s, "[trg] keycheck keys:%s | layout=%d ctx=0x%lX va=0x%llX", c, keyLayout1_ ? 1 : 2, (unsigned long)keyCtxPid_,
                          (unsigned long long)keyState_);
            gKeyLog(s);
        }
    }

    void resolveKey() const {
        keyTried_ = true;
        if (!vmm_ || !pid_) { keyInfo_ = "no vmm/pid"; return; }
        keyCands2_.clear();

        // enumerate pids + resolve the two host processes by name first
        std::vector<DWORD> pids;
        if (pidFromName_) {
            DWORD wp = 0;
            if (pidFromName_(vmm_, "winlogon.exe", &wp) && wp) pids.push_back(wp);
            if (pidFromName_(vmm_, "csrss.exe", &wp) && wp) pids.push_back(wp);
            if (gKeyLog) {
                char s[128];
                std::snprintf(s, sizeof s, "[trg] keyresolve pidFromName -> pids=%zu (%lu,%lu)", pids.size(),
                              pids.size() > 0 ? (unsigned long)pids[0] : 0ul, pids.size() > 1 ? (unsigned long)pids[1] : 0ul);
                gKeyLog(s);
            }
        }
        if (pidList_) {
            SIZE_T n = 0;
            if (pidList_(vmm_, nullptr, &n) && n > 1) {
                std::vector<DWORD> all(n);
                SIZE_T n2 = n;
                if (pidList_(vmm_, all.data(), &n2)) {
                    all.resize(n2);
                    for (DWORD p : all)
                        if (std::find(pids.begin(), pids.end(), p) == pids.end()) pids.push_back(p);
                }
            }
        }
        if (gKeyLog) {
            char s[128];
            std::snprintf(s, sizeof s, "[trg] keyresolve pids=%zu eatapi=%d modapi=%d", pids.size(), getEatU_ ? 1 : 0, moduleFromName_ ? 1 : 0);
            gKeyLog(s);
        }

        // win32kbase.sys is a session-space kernel mapping, so a process module list does not show it.
        // reliable way: base of win32kbase.sys in the kernel ctx (pid 4) + rva of gafAsyncKeyState from the PDB,
        // then re-base that rva for every plausible session ctx. The 7-key scan decides which one is live.
        uint64_t rva = 0;
        keyRva_ = 0;
        // image size of win32kbase: the refine stage needs it to arm module/pdb candidates.
        // Measured 2026-11-15 on the Win11 26100 target: module bases resolve in every ctx
        // (0xFFFFF8073E9D0000, cbImageSize 0x345000) but the blind session scan window does
        // not contain them, so "scan/session" candidates with a size never appear -> refine
        // had nothing to arm (0 images) and could never confirm an exact offset.
        uint32_t szKb = 0;
        if (moduleFromName_) {
            PVMMDLL_MAP_MODULEENTRY me = nullptr;
            if (moduleFromName_(vmm_, 4, "win32kbase.sys", &me, VMMDLL_MODULE_FLAG_NORMAL) && me) {
                szKb = me->cbImageSize;
                memFree_(me);
            }
        }
        const bool savedOk = loadSavedKeyArray();
        if (savedOk) keyCands2_.push_back({keyCtxPid_, keyState_, "saved"});
        if (procModBase_ && pdbLoad_ && pdbSymAddr_) {
            const DWORD kctx = 4;
            const uint64_t baseK = procModBase_(vmm_, kctx, "win32kbase.sys");
            if (baseK) {
                char nm[MAX_PATH] = {};
                if (pdbLoad_(vmm_, kctx, baseK, nm)) {
                    QWORD va = 0;
                    if (pdbSymAddr_(vmm_, nm, "gafAsyncKeyState", &va) && va > baseK) rva = static_cast<uint64_t>(va) - baseK;
                }
            }
            if (gKeyLog) {
                char s[200];
                std::snprintf(s, sizeof s, "[trg] keyresolve baseKernel=0x%llX rva=0x%llX (sdk ctx %s)", (unsigned long long)baseK,
                              (unsigned long long)rva, pids.empty() ? "-" : "ok");
                gKeyLog(s);
            }
            keyRva_ = rva;
            if (rva) {
                const DWORD wp = pids.empty() ? 0 : pids[0];
                const DWORD ctxs[] = {wp | 0x80000000u, 4u, wp, pid_ | 0x80000000u, pid_};
                for (const DWORD c : ctxs) {
                    const uint64_t base = (c == 4) ? baseK : procModBase_(vmm_, c, "win32kbase.sys");
                    if (gKeyLog) {
                        char s[160];
                        std::snprintf(s, sizeof s, "[trg] keyresolve base(ctx 0x%lX)=0x%llX", (unsigned long)c, (unsigned long long)base);
                        gKeyLog(s);
                    }
                    if (base) keyCands2_.push_back({c, base + rva, "pdb/win32kbase", szKb});
                    const bool inWin = baseK >= kWin32kScanLo && baseK < kWin32kScanHi;
                    if ((c == (wp | 0x80000000u) || c == 4u) && inWin) {
                        std::vector<std::pair<uint64_t, uint32_t>> bases;
                        scanWin32kbase(c, bases);
                        for (const auto& b : bases) keyCands2_.push_back({c, b.first + rva, "scan/session", b.second});
                        if (gKeyLog) {
                            char s[160];
                            std::snprintf(s, sizeof s, "[trg] keyresolve scan(ctx 0x%lX)=%zu images", (unsigned long)c, bases.size());
                            gKeyLog(s);
                        }
                    } else if (c == (wp | 0x80000000u) && gKeyLog) {
                        // measured 2026-11-15 (Win11 26100): module base 0xFFFFF807... lies outside the
                        // window, so the blind scan cost ~65k reads and could not find the image at all.
                        char s[176];
                        std::snprintf(s, sizeof s, "[trg] keyresolve blind scan skipped (baseK=0x%llX outside 0x%llX-0x%llX)",
                                      (unsigned long long)baseK, (unsigned long long)kWin32kScanLo, (unsigned long long)kWin32kScanHi);
                        gKeyLog(s);
                    }
                }
                // win32ksgd.sys bases (session-slot chain) for the csrss pids + module dump
                if (gKeyLog) {
                    for (const DWORD p : pids) {
                        PVMMDLL_MAP_MODULEENTRY me = nullptr;
                        if (!moduleFromName_ || !moduleFromName_(vmm_, p, "csrss.exe", &me, VMMDLL_MODULE_FLAG_NORMAL) || !me) continue;
                        memFree_(me);
                        const DWORD ctxs3[2] = {p, p | 0x80000000u};
                        for (const DWORD c : ctxs3) {
                            PVMMDLL_MAP_MODULE map = nullptr;
                            if (!getModuleU_ || !getModuleU_(vmm_, c, &map, VMMDLL_MODULE_FLAG_NORMAL) || !map) {
                                char s[128];
                                std::snprintf(s, sizeof s, "[trg] modlist ctx=0x%lX FAIL", (unsigned long)c);
                                gKeyLog(s);
                                continue;
                            }
                            char names[900] = {};
                            size_t at = 0;
                            for (DWORD i = 0; i < map->cMap && at < 850; ++i) {
                                const char* nm = map->pMap[i].uszText;
                                if (nm) at += size_t(std::snprintf(names + at, sizeof names - at, "%s|", nm));
                            }
                            char s[1100];
                            std::snprintf(s, sizeof s, "[trg] modlist ctx=0x%lX n=%lu sgd=0x%llX: %s", (unsigned long)c, (unsigned long)map->cMap,
                                          (unsigned long long)procModBase_(vmm_, c, "win32ksgd.sys"), names);
                            memFree_(map);
                            gKeyLog(s);
                        }
                    }
                }
                if (getEatU_ && gKeyLog) {
                    PVMMDLL_MAP_EAT e = nullptr;
                    if (getEatU_(vmm_, 4u, "ntoskrnl.exe", &e) && e) {
                        char s[240];
                        std::snprintf(s, sizeof s, "[trg] eatctl ntoskrnl dwVersion=%lu names=%lu funcs=%lu cMap=%lu vaBase=0x%llX", (unsigned long)e->dwVersion,
                                      (unsigned long)e->cNumberOfNames, (unsigned long)e->cNumberOfFunctions, (unsigned long)e->cMap,
                                      (unsigned long long)e->vaModuleBase);
                        gKeyLog(s);
                        memFree_(e);
                    } else {
                        gKeyLog("[trg] eatctl ntoskrnl FAILED");
                    }
                }
            }
        }
        // primary source: every process that actually has win32kbase.sys mapped (csrss per session, system)
        // -> module base + pdb rva and/or the export table in that exact context
        if (procModBase_) {
            int modCands = 0, eatCands = 0;
            for (const DWORD p : pids) {
                const DWORD ctxsP[2] = {p, p | 0x80000000u};
                for (const DWORD c : ctxsP) {
                    const uint64_t b = procModBase_(vmm_, c, "win32kbase.sys");
                    if (!b) continue;
                    if (rva) {
                        keyCands2_.push_back({c, b + rva, "mod/win32kbase", szKb});
                        ++modCands;
                    }
                    if (getEatU_) {
                        PVMMDLL_MAP_EAT eat = nullptr;
                        if (getEatU_(vmm_, c, "win32kbase.sys", &eat) && eat && eat->cMap) {
                            for (DWORD i = 0; i < eat->cMap; ++i) {
                                const char* fn = eat->pMap[i].uszFunction;
                                if (fn && _stricmp(fn, "gafAsyncKeyState") == 0 && eat->pMap[i].vaFunction) {
                                    keyCands2_.push_back({c, static_cast<uint64_t>(eat->pMap[i].vaFunction), "eat/win32kbase"});
                                    ++eatCands;
                                    break;
                                }
                            }
                        }
                        if (eat) memFree_(eat);
                    }
                }
            }
            if (gKeyLog) {
                char s[176];
                for (const auto& k : keyCands2_) {
                    std::snprintf(s, sizeof s, "[trg] keycand %s pid=0x%lX va=0x%llX", k.how, (unsigned long)k.pid, (unsigned long long)k.addr);
                    gKeyLog(s);
                }
            }
            if (gKeyLog) {
                char s[160];
                std::snprintf(s, sizeof s, "[trg] keyresolve modCands=%d eatCands=%d", modCands, eatCands);
                gKeyLog(s);
            }
        }
        // fallback: csrss.exe + win32ksgd.sys session-slot chain (if present on this build)
        if (moduleFromName_) {
            for (DWORD p : pids) {
                PVMMDLL_MAP_MODULEENTRY me = nullptr;
                if (!moduleFromName_(vmm_, p, "csrss.exe", &me, VMMDLL_MODULE_FLAG_NORMAL) || !me) continue;
                memFree_(me);
                uint64_t a = 0;
                DWORD c = 0;
                if (resolveViaCsrss(p, a, c)) keyCands2_.push_back({c, a, "sgd/csrss"});
            }
        }
        if (gKeyLog) {
            char s[128];
            std::snprintf(s, sizeof s, "[trg] keyresolve cands=%zu", keyCands2_.size());
            gKeyLog(s);
        }

        if (keyCands2_.empty()) {
            char b[128];
            std::snprintf(b, sizeof b, "no winlogon/csrss candidate (pids %zu, eatapi=%d)", pids.size(), getEatU_ ? 1 : 0);
            keyInfo_ = b;
            return;
        }
        keyCtxPid_ = keyCands2_[0].pid;
        keyState_ = keyCands2_[0].addr;
        keyHow_ = keyCands2_[0].how;
        keyPhase_ = 1;   // arm the 7-key verification automatically
        keyPhaseAt_ = GetTickCount64();
        keySnapA_.assign(keyCands2_.size() * 64, 0);
        for (size_t i = 0; i < keyCands2_.size(); ++i) readAt(keyCands2_[i].pid, keyCands2_[i].addr, keySnapA_.data() + i * 64, 64);
        {
            char b[192];
            std::snprintf(b, sizeof b, "ready via %s (ctxpid %lu, cands %zu, va 0x%llX)", keyHow_, (unsigned long)keyCtxPid_,
                          keyCands2_.size(), (unsigned long long)keyState_);
            keyInfo_ = b;
        }
    }

    mutable uint64_t keyWBase_ = 0;
    mutable uint32_t keyWSize_ = 0;
    // 0 idle, 1 armed 7-key scan, 4 differential refine (max 6 tries), 5 verified -> no periodic work
    mutable int keyPhase_ = 0;
    mutable ULONGLONG keyPhaseAt_ = 0;
    mutable std::vector<uint8_t> keyA_, keyB_;

    void readImage(std::vector<uint8_t>& out) const {
        out.assign(keyWSize_, 0);
        for (size_t o = 0; o < out.size(); o += 0x1000) {
            const DWORD n = DWORD(std::min<size_t>(0x1000, out.size() - o));
            DWORD got = 0;
            readEx_(vmm_, pid_ | 0x80000000, keyWBase_ + o, out.data() + o, n, &got, virtFlags());
        }
    }

    mutable std::vector<uint64_t> keyCands_;
    mutable std::vector<uint8_t> keySnapA_, keySnapB_;
    mutable uint64_t keyRva_ = 0;

    mutable std::vector<std::vector<uint8_t>> keyImgA_, keyImgB_;

    void findKey() const {
        if (!gKeyLog || keyCands2_.empty()) return;
        const ULONGLONG now = GetTickCount64();
        static const uint8_t vks[] = {0x10, 0x20, 0x41, 0x44, 0x45, 0x53, 0x57};
        if (keyPhase_ == 4) {
            if (now - keyPhaseAt_ < 5000) return;
            keyPhaseAt_ = now;
            const uint64_t t0 = qpcUs();
            if (refineCompare()) {  // confirmed -> no frame time spent on refining any more
                keyPhase_ = 5;
                if (gKeyLog) {
                    char s[128];
                    std::snprintf(s, sizeof s, "[trg] refine confirmed in %.0f ms - periodic refine stopped", (qpcUs() - t0) / 1000.0);
                    gKeyLog(s);
                }
                return;
            }
            if (++refineTries_ >= 6) {  // no confirmation -> back to the cheap watch loop (not "stop")
                keyScanRounds_ = 0;
                keyPhase_ = 0;
                keyPhaseAt_ = now;
                if (gKeyLog) {
                    char s[128];
                    std::snprintf(s, sizeof s, "[trg] refine gave up after %d tries (%.0f ms) - back to key watch", refineTries_,
                                  (qpcUs() - t0) / 1000.0);
                    gKeyLog(s);
                }
                findKey();
                return;
            }
            refineArm();
            return;
        }
        if (keyPhase_ == 5) return;
        if (keyPhase_ != 1) {
            // (re)arm: snapshot the idle state of every candidate
            keySnapA_.assign(keyCands2_.size() * 64, 0);
            for (size_t i = 0; i < keyCands2_.size(); ++i) readAt(keyCands2_[i].pid, keyCands2_[i].addr, keySnapA_.data() + i * 64, 64);
            char s[200];
            std::snprintf(s, sizeof s, "[trg] keyscan armed %zu cands - HOLD E+W+A+S+D+SPACE+SHIFT 5s", keyCands2_.size());
            gKeyLog(s);
            keyPhase_ = 1;
            keyPhaseAt_ = now;
            return;
        }
        if (now - keyPhaseAt_ < 5000) return;
        int bestNew = -1, bestDown = -1;
        size_t best = SIZE_MAX;
        bool bestLayout1 = false;
        for (size_t i = 0; i < keyCands2_.size(); ++i) {
            uint8_t raw[64] = {}, raw2[64] = {};
            if (!readAt(keyCands2_[i].pid, keyCands2_[i].addr, raw, 64)) continue;
            // idle-stability gate: a counter/timer byte changes within ~2 ms, a key bitmap does not
            if (!readAt(keyCands2_[i].pid, keyCands2_[i].addr, raw2, 64) || std::memcmp(raw, raw2, 64) != 0) {
                if (gKeyLog) {
                    char s[176];
                    std::snprintf(s, sizeof s, "[trg] keyscan %s skipped: window not idle-stable (counter/timer)", keyCands2_[i].how);
                    gKeyLog(s);
                }
                continue;
            }
            // score both layouts, keep the better one
            int downN = 0, newN = 0, downN1 = 0, newN1 = 0;
            for (const uint8_t k : vks) {
                const int bi2 = (k * 2) / 8, bo2 = (k % 4) * 2;
                if (raw[bi2] & (1 << bo2)) {
                    ++downN;
                    if (!(keySnapA_[i * 64 + bi2] & (1 << bo2))) ++newN;
                }
                const int bi1 = k >> 3, bo1 = k & 7;
                if (raw[bi1] & (1 << bo1)) {
                    ++downN1;
                    if (!(keySnapA_[i * 64 + bi1] & (1 << bo1))) ++newN1;
                }
            }
            const bool use1 = (newN1 * 4 + downN1) > (newN * 4 + downN);
            const int n = use1 ? newN1 : newN, d = use1 ? downN1 : downN;
            if (n > bestNew || (n == bestNew && d > bestDown)) {
                bestNew = n;
                bestDown = d;
                best = i;
                bestLayout1 = use1;
            }
            if (n > 0 || keyCands2_[i].how[0] != 's') {
                char s[176];
                std::snprintf(s, sizeof s, "[trg] keyscan %s pid=0x%lX va=0x%llX down=%d new=%d %s", keyCands2_[i].how, (unsigned long)keyCands2_[i].pid,
                              (unsigned long long)keyCands2_[i].addr, d, n, use1 ? "layout1" : "layout2");
                gKeyLog(s);
            }
        }
        if (best != SIZE_MAX && (bestNew > 0 || bestDown >= 4)) {
            char s[220];
            std::snprintf(s, sizeof s, "[trg] keyscan BEST via %s pid=0x%lX va=0x%llX down=%d new=%d %s", keyCands2_[best].how,
                          (unsigned long)keyCands2_[best].pid, (unsigned long long)keyCands2_[best].addr, bestDown, bestNew,
                          bestLayout1 ? "layout1" : "layout2");
            gKeyLog(s);
            keyCtxPid_ = keyCands2_[best].pid;
            keyState_ = keyCands2_[best].addr;
            keyHow_ = keyCands2_[best].how;
            keyLayout1_ = bestLayout1;
            keyVerified_ = true;
            keyDiag_ = false;      // re-print the raw bytes of the winning address
            keyPrevValid_ = false;
            keyInfo_ = std::string("tentative via ") + keyHow_;
            refineTries_ = 0;
            keyScanRounds_ = 0;
            keyPhase_ = 4;      // let the differential search confirm the exact offset
            keyPhaseAt_ = now;
            refineArm();
            return;
        }
        // not verified yet -> re-arm and try again (user can just press keys whenever)
        if (gKeyLog) {
            char s[128];
            std::snprintf(s, sizeof s, "[trg] keyscan no hit (new=%d down=%d) - re-arm, keep pressing", bestNew, bestDown);
            gKeyLog(s);
        }
        // nothing was pressed -> stay in the cheap 7-key watch loop (12 x 64 byte reads per 5 s).
        // Every 3rd round give the full-image differential a chance: it finds the array inside the
        // image even if the candidate VA (PDB rva) is off, which is the case on some builds.
        if (++keyScanRounds_ >= 3) {
            keyScanRounds_ = 0;
            refineTries_ = 0;
            keyPhase_ = 4;
            keyPhaseAt_ = now;
            refineArm();
            return;
        }
        keyPhase_ = 0;
        keyPhaseAt_ = now;
        findKey();
        return;
    }

    mutable std::vector<uint64_t> keyPtrs_;

    mutable std::vector<uint32_t> keySizes_;

    void readImg(uint64_t base, uint32_t size, std::vector<uint8_t>& out) const {
        out.assign(size, 0);
        for (size_t o = 0; o < out.size(); o += 0x1000) {
            const DWORD n = DWORD(std::min<size_t>(0x1000, out.size() - o));
            DWORD got = 0;
            readEx_(vmm_, pid_ | 0x80000000, base + o, out.data() + o, n, &got, virtFlags());
        }
    }

    void keyScan() const override {
        keyPhase_ = 0;  // re-arm (fresh idle snapshot) then start the 5s compare window
        findKey();
    }

    mutable uint64_t keyState_ = 0;
    mutable bool keyTried_ = false;
    mutable bool keyVerified_ = false;

    bool readVirt(uintptr_t addr, void* out, size_t size) const {
        DWORD got = 0;
        const BOOL ok = readEx_(vmm_, pid_, addr, static_cast<PBYTE>(out), static_cast<DWORD>(size), &got, virtFlags());
        return ok && got == size;
    }

    size_t readManyLocked(ReadReq* reqs, size_t count, std::vector<ReadReq*>* deferVirt = nullptr) const {
        if (physRead_) return readPhys(reqs, count, deferVirt);
        size_t n = 0;
        VMMDLL_SCATTER_HANDLE hs = scatterInit_ ? scatterInit_(vmm_, pid_, VMMDLL_FLAG_NOCACHE) : nullptr;
        if (!hs) {
            for (size_t i = 0; i < count; ++i) n += (reqs[i].ok = readVirt(reqs[i].addr, reqs[i].out, reqs[i].size)) ? 1 : 0;
            return n;
        }
        std::vector<DWORD> got(count, 0);
        for (size_t i = 0; i < count; ++i)
            scatterPrepareEx_(hs, reqs[i].addr, static_cast<DWORD>(reqs[i].size), static_cast<PBYTE>(reqs[i].out), &got[i]);
        const uint64_t t0 = qpcUs();
        scatterExecuteRead_(hs);
        tel::time(tel::T_DEVICE, qpcUs() - t0);
        tel::count(tel::C_SCATTERS);
        scatterClose_(hs);
        for (size_t i = 0; i < count; ++i) n += (reqs[i].ok = got[i] == reqs[i].size) ? 1 : 0;
        return n;
    }

    bool higherWaiting() const {
        for (int p = tlPrio + 1; p <= kPrioUrgent; ++p)
            if (waiting_[p].load(std::memory_order_relaxed) > 0) return true;
        return false;
    }
    void yieldToHigher() const {
        if (tlUrgent || tlHeld) return;
        if (!higherWaiting()) return;
        const uint64_t t0 = qpcUs();
        while (higherWaiting() && qpcUs() - t0 < 2000) std::this_thread::yield();
    }

    uint64_t vaToPa(uint64_t vaPage) const {
        auto it = pteCache_.find(vaPage);
        if (it != pteCache_.end()) return it->second;
        uint64_t pa = 0;
        if (!virt2phys_(vmm_, pid_, vaPage, &pa)) pa = 0;
        pa &= kPageMask;
        pteCache_.emplace(vaPage, pa);
        return pa;
    }

    size_t readPhys(ReadReq* reqs, size_t count, std::vector<ReadReq*>* deferVirt = nullptr) const {
        std::unordered_map<uint64_t, size_t> frameSlot;
        std::vector<uint64_t> frames;
        std::vector<char> needVirt(count, 0);

        const uint64_t tx0 = qpcUs();
        const size_t cached0 = pteCache_.size();
        size_t looks = 0;
        for (size_t i = 0; i < count; ++i) {
            reqs[i].ok = false;
            if (!reqs[i].size || !reqs[i].out) { reqs[i].ok = (reqs[i].size == 0); continue; }
            const uint64_t firstVa = uint64_t(reqs[i].addr) & kPageMask;
            const uint64_t lastVa = (uint64_t(reqs[i].addr) + reqs[i].size - 1) & kPageMask;
            for (uint64_t va = firstVa; va <= lastVa; va += kPageSize) {
                ++looks;
                const uint64_t pa = vaToPa(va);
                if (!pa) { needVirt[i] = 1; break; }
                if (frameSlot.emplace(pa, frames.size()).second) frames.push_back(pa);
            }
        }
        tel::time(tel::T_XLATE, qpcUs() - tx0);
        tel::count(tel::C_PTE_HIT, looks - (pteCache_.size() - cached0));
        tel::count(tel::C_PTE_MISS, pteCache_.size() - cached0);

        if (!frames.empty()) {
            std::vector<uint8_t> buf(frames.size() * kPageSize);
            std::vector<MEM_SCATTER> mems(frames.size());
            std::vector<PMEM_SCATTER> pp(frames.size());
            for (size_t s = 0; s < frames.size(); ++s) {
                MEM_SCATTER& m = mems[s];
                std::memset(&m, 0, sizeof m);
                m.version = MEM_SCATTER_VERSION;
                m.qwA = frames[s];
                m.pb = buf.data() + s * kPageSize;
                m.cb = static_cast<DWORD>(kPageSize);
                pp[s] = &m;
            }
            const uint64_t td0 = qpcUs();
            scatterRead_(vmm_, kPidPhysical, pp.data(), static_cast<DWORD>(pp.size()), VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOCACHEPUT);
            tel::time(tel::T_DEVICE, qpcUs() - td0);
            tel::count(tel::C_SCATTERS);
            tel::count(tel::C_PAGES, frames.size());
            uint64_t pagesFailed = 0;
            for (const MEM_SCATTER& m : mems) pagesFailed += m.f ? 0 : 1;
            tel::count(tel::C_PAGES_FAIL, pagesFailed);

            for (size_t i = 0; i < count; ++i) {
                if (needVirt[i] || !reqs[i].size) continue;
                uint8_t* dst = static_cast<uint8_t*>(reqs[i].out);
                uint64_t va = reqs[i].addr;
                size_t left = reqs[i].size;
                bool ok = true;
                while (left) {
                    const uint64_t pa = vaToPa(va & kPageMask);
                    const size_t off = static_cast<size_t>(va & 0xFFF);
                    const size_t n = std::min(left, static_cast<size_t>(kPageSize - off));
                    const auto slot = frameSlot.find(pa);
                    if (!pa || slot == frameSlot.end() || !mems[slot->second].f) { ok = false; break; }
                    std::memcpy(dst, buf.data() + slot->second * kPageSize + off, n);
                    dst += n; va += n; left -= n;
                }
                if (ok) reqs[i].ok = true; else needVirt[i] = 1;
            }
        }

        size_t ok = 0;
        for (size_t i = 0; i < count; ++i) {
            if (!reqs[i].ok && needVirt[i] && reqs[i].size && reqs[i].out) {
                if (deferVirt) {
                    deferVirt->push_back(&reqs[i]);
                    continue;
                }
                reqs[i].ok = readVirt(reqs[i].addr, reqs[i].out, reqs[i].size);
                tel::count(tel::C_VIRT_FB);
                if (!reqs[i].ok) tel::count(tel::C_VIRT_FB_FAIL);
            }
            if (reqs[i].ok) ++ok;
        }
        return ok;
    }

    size_t readVirtBatchLocked(ReadReq* const* rs, size_t count) const {
        size_t n = 0;
        VMMDLL_SCATTER_HANDLE hs = scatterInit_ ? scatterInit_(vmm_, pid_, virtFlags()) : nullptr;
        if (!hs) {
            for (size_t i = 0; i < count; ++i) n += (rs[i]->ok = readVirt(rs[i]->addr, rs[i]->out, rs[i]->size)) ? 1 : 0;
        } else {
            std::vector<DWORD> got(count, 0);
            for (size_t i = 0; i < count; ++i)
                scatterPrepareEx_(hs, rs[i]->addr, static_cast<DWORD>(rs[i]->size), static_cast<PBYTE>(rs[i]->out), &got[i]);
            const uint64_t t0 = qpcUs();
            scatterExecuteRead_(hs);
            tel::time(tel::T_VIRT, qpcUs() - t0);
            scatterClose_(hs);
            for (size_t i = 0; i < count; ++i) n += (rs[i]->ok = got[i] == rs[i]->size) ? 1 : 0;
        }
        tel::count(tel::C_VIRT_FB, count);
        tel::count(tel::C_VIRT_FB_FAIL, count - n);
        return n;
    }

    bool writeMemMap(const std::string& path) {
        const auto getPhysMem = reinterpret_cast<PfnGetPhysMem>(GetProcAddress(dll_, "VMMDLL_Map_GetPhysMem"));
        PVMMDLL_MAP_PHYSMEM map = nullptr;
        if (!getPhysMem || !getPhysMem(vmm_, &map) || !map) return false;
        bool ok = false;
        if (map->dwVersion == VMMDLL_MAP_PHYSMEM_VERSION && map->cMap > 0) {
            FILE* f = nullptr;
            if (!fopen_s(&f, path.c_str(), "w") && f) {
                for (DWORD i = 0; i < map->cMap; ++i)
                    std::fprintf(f, "%016llx - %016llx\n", static_cast<unsigned long long>(map->pMap[i].pa),
                                 static_cast<unsigned long long>(map->pMap[i].pa + map->pMap[i].cb - 1));
                ok = std::fclose(f) == 0;
            }
        }
        memFree_(map);
        return ok;
    }

    bool init(std::string& err) {
        std::vector<std::string> dirs;
        if (!dllDir_.empty()) dirs.push_back(dllDir_);
        dirs.push_back(exeDirectory() + "\\dma");
        dirs.push_back(exeDirectory());
        for (const auto& d : dirs) {
            const std::string path = d + "\\vmm.dll";
            if (fileExists(path)) {
                dll_ = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (dll_) break;
                err = "fatal: can't load " + path + " (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
        }
        if (!dll_) dll_ = LoadLibraryA("vmm.dll");
        if (!dll_) {
            err = "fatal: vmm.dll not found";
            return false;
        }

        auto sym = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(dll_, name)); };
        initialize_ = reinterpret_cast<PfnInitialize>(sym("VMMDLL_Initialize"));
        close_ = reinterpret_cast<PfnClose>(sym("VMMDLL_Close"));
        memFree_ = reinterpret_cast<PfnMemFree>(sym("VMMDLL_MemFree"));
        pidFromName_ = reinterpret_cast<PfnPidGetFromName>(sym("VMMDLL_PidGetFromName"));
        readEx_ = reinterpret_cast<PfnMemReadEx>(sym("VMMDLL_MemReadEx"));
        moduleFromName_ = reinterpret_cast<PfnGetModuleFromNameU>(sym("VMMDLL_Map_GetModuleFromNameU"));
        getEatU_ = reinterpret_cast<PfnGetEatU>(sym("VMMDLL_Map_GetEATU"));
        pidList_ = reinterpret_cast<PfnPidList>(sym("VMMDLL_PidList"));
        procModBase_ = reinterpret_cast<PfnProcessGetModuleBaseU>(sym("VMMDLL_ProcessGetModuleBaseU"));
        vadU_ = reinterpret_cast<PfnGetVadU>(sym("VMMDLL_Map_GetVadU"));
        if (!initialize_ || !close_ || !memFree_ || !pidFromName_ || !readEx_ || !moduleFromName_ || !vadU_) {
            err = "fatal: wrong vmm.dll version";
            return false;
        }
        scatterInit_ = reinterpret_cast<PfnScatterInitialize>(sym("VMMDLL_Scatter_Initialize"));
        scatterPrepareEx_ = reinterpret_cast<PfnScatterPrepareEx>(sym("VMMDLL_Scatter_PrepareEx"));
        scatterExecuteRead_ = reinterpret_cast<PfnScatterExecuteRead>(sym("VMMDLL_Scatter_ExecuteRead"));
        scatterClose_ = reinterpret_cast<PfnScatterCloseHandle>(sym("VMMDLL_Scatter_CloseHandle"));
        if (!scatterPrepareEx_ || !scatterExecuteRead_ || !scatterClose_) scatterInit_ = nullptr;
        scatterRead_ = reinterpret_cast<PfnMemReadScatter>(sym("VMMDLL_MemReadScatter"));
        virt2phys_ = reinterpret_cast<PfnVirt2Phys>(sym("VMMDLL_MemVirt2Phys"));
        physRead_ = scatterRead_ && virt2phys_;
        pdbLoad_ = reinterpret_cast<PfnPdbLoad>(sym("VMMDLL_PdbLoad"));
    pdbSymAddr_ = reinterpret_cast<PfnPdbSymbolAddress>(sym("VMMDLL_PdbSymbolAddress"));
    getModuleU_ = reinterpret_cast<PfnGetModuleU>(sym("VMMDLL_Map_GetModuleU"));
    configSet_ = reinterpret_cast<PfnConfigSet>(sym("VMMDLL_ConfigSet"));
        if (!configSet_) ownRefresh_ = false;

        std::string dev = device_.empty() ? "fpga" : device_;
        const std::string mmap = exeDirectory() + "\\mmap.txt";
        const auto open = [&](const std::string& memmap) {
            std::vector<LPCSTR> argv = {"", "-device", dev.c_str(), "-waitinitialize"};
            if (!memmap.empty()) {
                argv.push_back("-memmap");
                argv.push_back(memmap.c_str());
            }
            if (ownRefresh_) argv.push_back("-norefresh");
            return initialize_(DWORD(argv.size()), argv.data());
        };
        bool haveMap = fileExists(mmap);
        if (haveMap) vmm_ = open(mmap);
        if (!vmm_) {
            vmm_ = open("");
            if (vmm_ && haveMap) DeleteFileA(mmap.c_str());
            haveMap = false;
        }
        if (vmm_ && !haveMap && writeMemMap(mmap)) {
            close_(vmm_);
            vmm_ = open(mmap);
            haveMap = vmm_ != nullptr;
            if (!vmm_) {
                vmm_ = open("");
                if (vmm_) DeleteFileA(mmap.c_str());
            }
        }
        lastTlb_ = lastPteFlush_ = GetTickCount64();
        if (!vmm_) {
            err = "fatal: MemProcFS init failed (" + dev + ")";
            return false;
        }
        return true;
    }

    std::string device_, dllDir_;
    bool ownRefresh_ = false;
    bool noPaging_ = false;
    mutable bool pagingOk_ = false;
    ULONGLONG lastProcRefresh_ = 0;
    bool physRead_ = false;
    ULONGLONG lastTlb_ = 0, lastPteFlush_ = 0;
    mutable std::unordered_map<uint64_t, uint64_t> pteCache_;
    std::vector<AliveProbe> probes_;
    mutable std::recursive_mutex mx_;
    mutable std::atomic<int> waiting_[3] = {};
    PfnConfigSet configSet_ = nullptr;
    HMODULE dll_ = nullptr;
    VMM_HANDLE vmm_ = nullptr;
    PfnInitialize initialize_ = nullptr;
    PfnClose close_ = nullptr;
    PfnMemFree memFree_ = nullptr;
    PfnPidGetFromName pidFromName_ = nullptr;
    PfnMemReadEx readEx_ = nullptr;
    PfnPdbLoad pdbLoad_ = nullptr;
    PfnPdbSymbolAddress pdbSymAddr_ = nullptr;
    PfnGetModuleU getModuleU_ = nullptr;
    PfnGetModuleFromNameU moduleFromName_ = nullptr;
    PfnGetEatU getEatU_ = nullptr;
    PfnPidList pidList_ = nullptr;
    PfnProcessGetModuleBaseU procModBase_ = nullptr;
    PfnGetVadU vadU_ = nullptr;
    PfnScatterInitialize scatterInit_ = nullptr;
    PfnScatterPrepareEx scatterPrepareEx_ = nullptr;
    PfnScatterExecuteRead scatterExecuteRead_ = nullptr;
    PfnScatterCloseHandle scatterClose_ = nullptr;
    PfnMemReadScatter scatterRead_ = nullptr;
    PfnVirt2Phys virt2phys_ = nullptr;
};

}

void markUrgentReadThread() {
    tlPrio = kPrioUrgent;
    tlUrgent = true;
}
void markBackgroundReadThread() { tlPrio = kPrioBg; }

void setDmaKeyLog(void (*fn)(const char*)) { gKeyLog = fn; }

std::unique_ptr<Process> makeDmaProcess(const std::string& device, const std::string& dllDir, bool ownRefresh) {
    return std::make_unique<DmaProcess>(device, dllDir, ownRefresh);
}
