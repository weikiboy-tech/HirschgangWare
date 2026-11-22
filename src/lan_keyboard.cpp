#include "lan_keyboard_wire.h"
#include "lan_keyboard.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace lan_keyboard {
namespace {
using namespace wire;

enum class State { Disabled, Invalid, ListenError, Waiting, Connected };
std::atomic<State> state{State::Disabled};
std::atomic<bool> stopping{false};
std::thread worker;
std::mutex snapshotMutex;
dma_keyboard::Sample snapshot;
uint64_t receivedAt = 0;

void publish(dma_keyboard::Sample value = {}) {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    if (dma_keyboard::sameSource(snapshot, value)) {
        value.edges = snapshot.edges;
        for (unsigned byte = 2; byte < value.bits.size(); ++byte) {
            if (snapshot.bits[byte] == value.bits[byte]) continue;
            for (unsigned key = byte * 4; key < byte * 4 + 4 && dma_keyboard::validKey(key); ++key) {
                const bool held = dma_keyboard::down(value.bits, key);
                if (held == dma_keyboard::down(snapshot.bits, key)) continue;
                auto& counts = value.edges[key];
                if (held) ++counts.pressed;
                else ++counts.released;
            }
        }
    }
    snapshot = value;
    receivedAt = value.pid ? GetTickCount64() : 0;
}

void receive(sockaddr_in address, const Key& key, void (*log)(const char*)) {
    Winsock network;
    Cipher cipher(key);
    Socket listener(network ? socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) : INVALID_SOCKET);
    const BOOL exclusive = TRUE;
    if (!cipher || !listener ||
        setsockopt(listener.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive) ||
        bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) || listen(listener.get(), 1)) {
        state = State::ListenError;
        if (log) log("[input] LAN keyboard listener failed. Check the configured IP and port.");
        return;
    }
    state = State::Waiting;
    if (log) log("[input] LAN keyboard listener ready");
    uint32_t session = 0;
    while (!stopping) {
        if (!waitSocket(listener.get(), false, GetTickCount64() + 60)) continue;
        Socket connection(accept(listener.get(), nullptr, nullptr));
        if (!connection) continue;
        connection.noDelay();
        Challenge challenge{};
        std::copy(kMagic.begin(), kMagic.end(), challenge.begin());
        if (!randomBytes(challenge.data() + kMagic.size(), challenge.size() - kMagic.size()) ||
            !transfer(connection.get(), challenge, true, kTimeoutMs)) continue;
        if (++session == 0) ++session;
        for (uint64_t seq = 1; !stopping && seq; ++seq) {
            Packet packet{};
            dma_keyboard::Sample next{session, {}, dma_keyboard::Transport::Lan};
            if (!transfer(connection.get(), packet, false, kTimeoutMs) ||
                !cipher.open(challenge, seq, packet, next.bits)) break;
            publish(next);
            if (seq == 1) {
                state = State::Connected;
                if (log) log("[input] LAN keyboard authenticated");
            }
        }
        publish();
        if (state.exchange(State::Waiting) == State::Connected && log)
            log("[input] LAN keyboard disconnected; held state cleared");
    }
}
}  // namespace

void start(const std::string& path, void (*log)(const char*)) {
    if (worker.joinable() || GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    char ip[40]{}, hex[80]{};
    GetPrivateProfileStringA("lan_keyboard", "listen", "", ip, sizeof ip, path.c_str());
    GetPrivateProfileStringA("lan_keyboard", "key", "", hex, sizeof hex, path.c_str());
    const unsigned port = GetPrivateProfileIntA("lan_keyboard", "port", kDefaultPort, path.c_str());
    Key key{};
    sockaddr_in address{};
    const bool valid = parseKey(hex, key) && privateEndpoint(ip, port, address);
    SecureZeroMemory(hex, sizeof hex);
    if (!valid) {
        state = State::Invalid;
        if (log) log("[input] LAN keyboard receiver configuration is invalid");
        return;
    }
    stopping = false;
    try {
        worker = std::thread(receive, address, key, log);
    } catch (const std::system_error&) {
        state = State::ListenError;
        if (log) log("[input] LAN keyboard worker could not start");
    }
    SecureZeroMemory(key.data(), key.size());
}

void stop() {
    stopping = true;
    if (worker.joinable()) worker.join();
    publish();
    state = State::Disabled;
}

dma_keyboard::Sample sample() {
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return receivedAt && GetTickCount64() - receivedAt < kTimeoutMs ? snapshot : dma_keyboard::Sample{};
}

const char* status() {
    switch (state.load()) {
        case State::Invalid: return "LAN keyboard: invalid configuration";
        case State::ListenError: return "LAN keyboard: IP or port unavailable";
        case State::Waiting: return "LAN keyboard: waiting for main PC";
        case State::Connected: return sample().pid ? "LAN keyboard: connected" : "LAN keyboard: waiting for main PC";
        default: return "LAN keyboard: not configured";
    }
}
}  // namespace lan_keyboard
