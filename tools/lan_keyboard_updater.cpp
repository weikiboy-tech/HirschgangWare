#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "lan_keyboard_update.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "user32.lib")

namespace {
using std::filesystem::path;
constexpr wchar_t kSenderFile[] = L"HirschgangWare-InputSender.exe";
constexpr wchar_t kHost[] = L"raw.githubusercontent.com";
constexpr wchar_t kManifest[] = L"/weikiboy-tech/HirschgangWare/main/dist/lan-keyboard-version.txt";
constexpr wchar_t kPayload[] = L"/weikiboy-tech/HirschgangWare/main/dist/HirschgangWare-InputSender.exe";
constexpr size_t kMaxPayload = 2 * 1024 * 1024;
constexpr DWORD kIdleMs = 60000;
constexpr DWORD kIntervalMs = 60 * 60 * 1000;

struct CloseHandleDeleter { void operator()(void* h) const { CloseHandle(h); } };
struct CloseHttpDeleter { void operator()(void* h) const { WinHttpCloseHandle(h); } };
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
using Http = std::unique_ptr<void, CloseHttpDeleter>;

void log(const path& directory, const std::string& message) {
    std::ofstream(directory / "lan-keyboard-update.log", std::ios::trunc) << message << '\n';
}

std::string fetch(const wchar_t* route, size_t limit) {
    Http session{WinHttpOpen(L"HirschgangWare Input Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session || !WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 3000)) return {};
    Http host{WinHttpConnect(session.get(), kHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!host) return {};
    Http request{WinHttpOpenRequest(host.get(), L"GET", route, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH)};
    DWORD features = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof features) ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) return {};
    DWORD status = 0, length = sizeof status;
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) return {};
    const auto deadline = GetTickCount64() + 8000;
    std::string bytes;
    char chunk[8192];
    for (;;) {
        DWORD read = 0;
        if (GetTickCount64() > deadline || !WinHttpReadData(request.get(), chunk, sizeof chunk, &read) ||
            bytes.size() + read > limit) return {};
        if (!read) return bytes;
        bytes.append(chunk, read);
    }
}

unsigned revisionOf(const path& file) {
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size || size > 65536) return 0;
    std::vector<uint8_t> bytes(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoBytes = 0;
    if (!GetFileVersionInfoW(file.c_str(), 0, size, bytes.data()) ||
        !VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &infoBytes) ||
        infoBytes < sizeof *info || info->dwSignature != VS_FFI_SIGNATURE ||
        info->dwFileVersionMS != 0x00010000 || HIWORD(info->dwFileVersionLS)) return 0;
    return LOWORD(info->dwFileVersionLS);
}

bool matchesRelease(std::string& payload, const lan_update::Release& release) {
    if (payload.empty()) return false;
    std::array<uint8_t, 32> sha{};
    return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<uint8_t*>(payload.data()),
                      static_cast<ULONG>(payload.size()), sha.data(), static_cast<ULONG>(sha.size())) >= 0 &&
           sha == release.sha256;
}

HWND readyWindow(HANDLE sender) {
    const auto window = FindWindowW(lan_update::kSenderWindow, nullptr);
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window, &pid);
    return pid && pid == GetProcessId(sender) && GetPropW(window, lan_update::kReadyProperty) ? window : nullptr;
}

Handle launch(const path& sender) {
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(sender.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                        sender.parent_path().c_str(), &startup, &child)) return {};
    CloseHandle(child.hThread);
    return Handle(child.hProcess);
}

bool verifyReady(HANDLE sender) {
    const auto deadline = GetTickCount64() + 4000;
    while (GetTickCount64() < deadline && WaitForSingleObject(sender, 40) == WAIT_TIMEOUT) {
        if (readyWindow(sender)) return true;
    }
    return false;
}

bool stopFailedChild(const Handle& child) {
    if (!child || WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0) return true;
    TerminateProcess(child.get(), 1);
    return WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0;
}

bool restoreSender(const path& previous, const path& exe, Handle& sender) {
    if (!revisionOf(previous) || !stopFailedChild(sender) ||
        !MoveFileExW(previous.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    sender = launch(exe);
    return sender && verifyReady(sender.get());
}

bool isIdle() {
    LASTINPUTINFO info{sizeof info, 0};
    return GetLastInputInfo(&info) && GetTickCount() - info.dwTime >= kIdleMs;
}

// The loop never waits on network I/O from the sender's Raw Input thread.
int supervise(const path& directory, bool checkOnly) {
    const path exe = directory / kSenderFile;
    const path staging = directory / "sender-download.tmp";
    const path previous = directory / "sender-previous.exe";
    unsigned revision = revisionOf(exe);
    unsigned failedRevision = 0;
    Handle sender;
    if (checkOnly) {
        if (!revision) { log(directory, "No versioned sender installed."); return 1; }
    } else {
        if (revision) sender = launch(exe);
        if (!sender || !verifyReady(sender.get())) {
            failedRevision = revision;
            if (!restoreSender(previous, exe, sender)) { log(directory, "Sender startup failed."); return 1; }
            revision = revisionOf(exe);
            log(directory, "Restored the previous sender after a startup failure.");
        }
    }
    do {
        const auto release = lan_update::parseRelease(fetch(kManifest, 128));
        if (!release) {
            log(directory, "Update check failed. Installed sender was kept.");
            if (checkOnly) return 1;
        } else if (!checkOnly && (release->revision <= revision || release->revision == failedRevision)) {
            log(directory, "Installed sender revision " + std::to_string(revision) + ".");
        } else {
            const auto route = std::wstring(kPayload) + L"?revision=" + std::to_wstring(release->revision);
            auto payload = fetch(route.c_str(), kMaxPayload);
            bool staged = false;
            if (matchesRelease(payload, *release)) {
                std::ofstream out(staging, std::ios::binary | std::ios::trunc);
                out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
                out.close();
                staged = out && revisionOf(staging) == release->revision;
            }
            if (!staged) {
                DeleteFileW(staging.c_str());
                log(directory, "Update download failed verification. Installed sender was kept.");
                if (checkOnly) return 1;
            } else if (checkOnly) {
                DeleteFileW(staging.c_str());
                log(directory, "Verified sender update to revision " + std::to_string(release->revision) + ".");
            } else {
                log(directory, "Update verified. Waiting for one minute of input inactivity.");
                while (!isIdle()) {
                    if (WaitForSingleObject(sender.get(), 1000) != WAIT_TIMEOUT) return 0;
                }
                const auto window = readyWindow(sender.get());
                if (window && CopyFileW(exe.c_str(), previous.c_str(), FALSE) &&
                    PostMessageW(window, WM_CLOSE, 0, 0) && WaitForSingleObject(sender.get(), 5000) == WAIT_OBJECT_0) {
                    sender.reset();
                    const bool replaced = MoveFileExW(staging.c_str(), exe.c_str(),
                                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                    sender = launch(exe);
                    if (replaced && sender && verifyReady(sender.get())) {
                        revision = release->revision;
                        log(directory, "Updated sender to revision " + std::to_string(revision) + ".");
                    } else {
                        failedRevision = release->revision;
                        if (replaced) {
                            if (!restoreSender(previous, exe, sender)) {
                                log(directory, "Update startup failed and the automatic restore did not complete.");
                                return 1;
                            }
                        }
                        log(directory, "Update failed to start. Retained the previous sender revision.");
                        if (!sender || !verifyReady(sender.get())) return 1;
                    }
                } else {
                    log(directory, "Update deferred. Sender could not be stopped cleanly.");
                }
                DeleteFileW(staging.c_str());
            }
        }
    } while (!checkOnly && WaitForSingleObject(sender.get(), kIntervalMs) == WAIT_TIMEOUT);
    return 0;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int) {
    Handle lock{CreateMutexW(nullptr, FALSE, L"Local\\HirschgangWareInputUpdater")};
    if (!lock || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    wchar_t file[32768]{};
    const auto size = GetModuleFileNameW(nullptr, file, static_cast<DWORD>(std::size(file)));
    if (!size || size >= std::size(file)) return 1;
    return supervise(path(file).parent_path(), std::wstring_view(command) == L"--check-only");
}
