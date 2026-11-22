#include "../tools/lan_keyboard_updater.cpp"
#include <cassert>
#include <cstdio>

static LRESULT CALLBACK testWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(window, message, wp, lp);
}

static int runFixture() {
    WNDCLASSW info{};
    info.hInstance = GetModuleHandleW(nullptr);
    info.lpszClassName = lan_update::kSenderWindow;
    info.lpfnWndProc = testWindow;
    assert(RegisterClassW(&info));
    const auto window = CreateWindowExW(0, info.lpszClassName, L"LAN updater test", 0, 0, 0, 0, 0,
                                        nullptr, nullptr, info.hInstance, nullptr);
    assert(window && SetPropW(window, lan_update::kReadyProperty, reinterpret_cast<HANDLE>(1)));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    return 0;
}

int main() {
    wchar_t filename[32768]{};
    assert(GetModuleFileNameW(nullptr, filename, static_cast<DWORD>(std::size(filename))));
    const path current(filename);
    if (current.filename() == kSenderFile) return runFixture();

    auto knownSha = lan_update::parseRelease("4 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string payload = "abc";
    assert(knownSha && matchesRelease(payload, *knownSha));
    payload[1] = 'B';
    assert(!matchesRelease(payload, *knownSha));

    const auto directory = std::filesystem::temp_directory_path() /
        ("hirschgang-input-updater-" + std::to_string(GetCurrentProcessId()));
    assert(std::filesystem::create_directory(directory));
    const auto installed = directory / kSenderFile;
    const auto previous = directory / "sender-previous.exe";
    assert(std::filesystem::copy_file(current, previous));
    std::ofstream(installed) << "Broken sender";
    assert(!launch(installed));
    Handle sender;
    assert(restoreSender(previous, installed, sender));
    assert(revisionOf(installed) == revisionOf(current));
    assert(readyWindow(sender.get()));
    assert(PostMessageW(readyWindow(sender.get()), WM_CLOSE, 0, 0));
    assert(WaitForSingleObject(sender.get(), 5000) == WAIT_OBJECT_0);
    std::filesystem::remove(installed);
    std::filesystem::remove(directory);
    puts("PASS updater: SHA-256 mismatch rejected, previous executable restored and launched");
}
