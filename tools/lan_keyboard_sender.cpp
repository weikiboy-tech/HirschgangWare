#include "../src/lan_keyboard_wire.h"
#include "../src/lan_key_outbox.h"
#include "../src/lan_keyboard_keys.h"
#include "lan_keyboard_update.h"
#include <wtsapi32.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace {
using namespace lan_keyboard::wire;
using dma_keyboard::Bitmap;

struct DeviceKeys { HANDLE device = nullptr; Bitmap bits{}; };
std::array<DeviceKeys, 16> keyboards;
Bitmap allowed;
bool paused = false;
std::atomic<bool> stopping{false};
std::mutex snapshotMutex;
std::condition_variable snapshotChanged;
lan_keyboard::Outbox outbox;

void publish() {
    Bitmap bits{};
    if (!paused) {
        for (const auto& keyboard : keyboards) {
            for (size_t i = 0; i < bits.size(); ++i) bits[i] |= keyboard.bits[i];
        }
    }
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (outbox.publish(bits, GetTickCount64())) snapshotChanged.notify_one();
}

void input(HRAWINPUT handle) {
    RAWINPUT raw{};
    UINT size = sizeof raw;
    if (paused || GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
        raw.header.dwType != RIM_TYPEKEYBOARD || !raw.header.hDevice) return;
    const auto& key = raw.data.keyboard;
    unsigned vk = key.VKey;
    if (vk == VK_SHIFT) vk = MapVirtualKeyW(key.MakeCode, MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) vk = key.Flags & RI_KEY_E0 ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) vk = key.Flags & RI_KEY_E0 ? VK_RMENU : VK_LMENU;
    if (!dma_keyboard::validKey(vk) || !dma_keyboard::down(allowed, vk)) return;

    auto device = std::find_if(keyboards.begin(), keyboards.end(), [&](const DeviceKeys& d) {
        return d.device == raw.header.hDevice;
    });
    const bool down = !(key.Flags & RI_KEY_BREAK);
    if (device == keyboards.end() && down) {
        device = std::find_if(keyboards.begin(), keyboards.end(), [](const DeviceKeys& d) { return !d.device; });
        if (device != keyboards.end()) device->device = raw.header.hDevice;
    }
    if (device == keyboards.end()) return;
    lan_keyboard::setKey(device->bits, vk, down);
    if (device->bits == Bitmap{}) *device = {};
    publish();
}

LRESULT CALLBACK inputWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INPUT) input(reinterpret_cast<HRAWINPUT>(lp));
    else if (message == WM_INPUT_DEVICE_CHANGE && wp == GIDC_REMOVAL) {
        for (auto& keyboard : keyboards) if (keyboard.device == reinterpret_cast<HANDLE>(lp)) keyboard = {};
        publish();
    } else if (message == WM_WTSSESSION_CHANGE) {
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_LOGOFF || wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT)
            paused = true;
        else if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)
            paused = false;
        keyboards = {};
        publish();
    } else if (message == WM_TIMER) publish();
    else if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(window, message, wp, lp);
}

void sendStates(const Socket& connection, const Challenge& challenge, const Cipher& cipher) {
    constexpr uint64_t maxBacklogMs = 150;
    Bitmap bits{};
    {
        std::lock_guard<std::mutex> lock(snapshotMutex);
        if (!outbox.begin(GetTickCount64(), maxBacklogMs, bits)) return;
    }
    for (uint64_t sequence = 1; !stopping && sequence; ++sequence) {
        Packet packet{};
        if (!cipher.seal(challenge, sequence, bits, packet) ||
            !transfer(connection.get(), packet, true, maxBacklogMs)) return;
        std::unique_lock<std::mutex> lock(snapshotMutex);
        snapshotChanged.wait_for(lock, std::chrono::milliseconds(kHeartbeatMs), [] { return stopping || outbox.pending(); });
        if (!outbox.next(GetTickCount64(), maxBacklogMs, bits)) return;
    }
}

void sendLoop(const sockaddr_in address, const Cipher& cipher) {
    while (!stopping) {
        {
            Socket connection(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            Challenge challenge{};
            if (connection && connectTo(connection.get(), address) &&
                transfer(connection.get(), challenge, false, 750) &&
                std::equal(kMagic.begin(), kMagic.end(), challenge.begin())) {
                connection.noDelay();
                sendStates(connection, challenge, cipher);
            }
        }
        for (int i = 0; i < 10 && !stopping; ++i) Sleep(50);
    }
}

int fail(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"HirschgangWare LAN-Tastatur", MB_OK | MB_ICONERROR);
    return 1;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\HirschgangWareInputSender");
    if (!singleton) return fail(L"Die Sender-Instanz konnte nicht angelegt werden.");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }
    wchar_t exe[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!length || length >= std::size(exe)) return fail(L"Die Sender-Konfiguration konnte nicht gefunden werden.");
    const auto config = std::filesystem::path(exe).parent_path() / L"lan-keyboard-sender.ini";
    wchar_t receiver[40]{}, hex[80]{}, keys[160]{};
    GetPrivateProfileStringW(L"lan_keyboard", L"receiver", L"", receiver, DWORD(std::size(receiver)), config.c_str());
    GetPrivateProfileStringW(L"lan_keyboard", L"key", L"", hex, DWORD(std::size(hex)), config.c_str());
    GetPrivateProfileStringW(L"lan_keyboard", L"keys", L"", keys, DWORD(std::size(keys)), config.c_str());
    const unsigned port = GetPrivateProfileIntW(L"lan_keyboard", L"port", kDefaultPort, config.c_str());
    const auto ascii = [](const wchar_t* s) {
        std::string value;
        for (; *s; ++s) {
            if (*s > 127) return std::string{};
            value.push_back(static_cast<char>(*s));
        }
        return value;
    };
    sockaddr_in address{};
    Key secret{};
    Winsock network;
    if (!network || !privateEndpoint(ascii(receiver).c_str(), port, address) ||
        !parseKey(ascii(hex), secret) || !lan_keyboard::parseAllowedKeys(ascii(keys), allowed))
        return fail(L"Ungültige lan-keyboard-sender.ini. Das Paket am Zweit-PC neu erzeugen.");
    SecureZeroMemory(hex, sizeof hex);
    const Cipher cipher(secret);
    SecureZeroMemory(secret.data(), secret.size());
    if (!cipher) return fail(L"Windows-Verschlüsselung konnte nicht gestartet werden.");

    WNDCLASSW wc{};
    wc.lpfnWndProc = inputWindow;
    wc.hInstance = instance;
    wc.lpszClassName = lan_update::kSenderWindow;
    if (!RegisterClassW(&wc)) return fail(L"Das Raw-Input-Fenster konnte nicht registriert werden.");
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"HirschgangWare LAN-Tastatur", 0,
                                  0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    const RAWINPUTDEVICE keyboard{1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
    if (!window || !RegisterRawInputDevices(&keyboard, 1, sizeof keyboard) ||
        !WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) || !SetTimer(window, 1, 40, nullptr))
        return fail(L"Windows-Tastatureingang konnte nicht registriert werden.");

    publish();
    std::thread sender(sendLoop, address, std::cref(cipher));
    SetPropW(window, lan_update::kReadyProperty, reinterpret_cast<HANDLE>(1));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    stopping = true;
    snapshotChanged.notify_one();
    sender.join();
    WTSUnRegisterSessionNotification(window);
    CloseHandle(singleton);
    return 0;
}
