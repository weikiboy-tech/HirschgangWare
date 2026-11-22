#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include "keyboard_bitmap.h"
#include <algorithm>
#include <array>
#include <string_view>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace lan_keyboard::wire {

constexpr unsigned kDefaultPort = 47832;
constexpr uint64_t kHeartbeatMs = 45;
constexpr uint64_t kTimeoutMs = 220;
constexpr std::array<uint8_t, 8> kMagic{'H','G','K','E','Y','S',1,0};
using Key = std::array<uint8_t, 32>;
using Challenge = std::array<uint8_t, kMagic.size() + 24>;
using Plain = std::array<uint8_t, 8 + dma_keyboard::Bitmap{}.size()>;
using Packet = std::array<uint8_t, 12 + Plain{}.size() + 16>;

inline bool parseKey(std::string_view hex, Key& key) {
    if (hex.size() != key.size() * 2) return false;
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < key.size(); ++i) {
        const int hi = digit(hex[i * 2]), lo = digit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        key[i] = uint8_t((hi << 4) | lo);
    }
    return std::any_of(key.begin(), key.end(), [](uint8_t v) { return v != 0; });
}

inline bool randomBytes(uint8_t* bytes, size_t length) {
    return BCryptGenRandom(nullptr, bytes, ULONG(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

class Cipher {
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_KEY_HANDLE key_ = nullptr;

    bool crypt(bool encrypt, Challenge challenge, Packet& packet, Plain& plain) const {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = packet.data();
        info.cbNonce = 12;
        info.pbAuthData = challenge.data();
        info.cbAuthData = ULONG(challenge.size());
        info.pbTag = packet.data() + 12 + plain.size();
        info.cbTag = 16;
        ULONG size = 0;
        const auto status = encrypt
            ? BCryptEncrypt(key_, plain.data(), ULONG(plain.size()), &info, nullptr, 0,
                packet.data() + 12, ULONG(plain.size()), &size, 0)
            : BCryptDecrypt(key_, packet.data() + 12, ULONG(plain.size()), &info, nullptr, 0,
                plain.data(), ULONG(plain.size()), &size, 0);
        return status >= 0 && size == plain.size();
    }

public:
    explicit Cipher(Key key) {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptSetProperty(alg_, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                ULONG(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) >= 0) {
            if (BCryptGenerateSymmetricKey(alg_, &key_, nullptr, 0, key.data(), ULONG(key.size()), 0) < 0)
                key_ = nullptr;
        }
        SecureZeroMemory(key.data(), key.size());
    }
    ~Cipher() {
        if (key_) BCryptDestroyKey(key_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Cipher(const Cipher&) = delete;
    Cipher& operator=(const Cipher&) = delete;
    explicit operator bool() const { return key_ != nullptr; }

    bool seal(const Challenge& challenge, uint64_t sequence, const dma_keyboard::Bitmap& bitmap, Packet& packet) const {
        Plain plain{};
        for (size_t i = 0; i < 8; ++i) plain[i] = uint8_t(sequence >> (8 * i));
        std::copy(bitmap.begin(), bitmap.end(), plain.begin() + 8);
        return key_ && randomBytes(packet.data(), 12) && crypt(true, challenge, packet, plain);
    }

    bool open(const Challenge& challenge, uint64_t expected, Packet& packet, dma_keyboard::Bitmap& bitmap) const {
        Plain plain{};
        if (!key_ || !crypt(false, challenge, packet, plain)) return false;
        uint64_t sequence = 0;
        for (size_t i = 0; i < 8; ++i) sequence |= uint64_t(plain[i]) << (8 * i);
        if (sequence != expected || plain[8] || plain[9] || (plain.back() & 0xc0)) return false;
        for (size_t i = 8; i < plain.size(); ++i) if (plain[i] & 0xaa) return false;
        std::copy(plain.begin() + 8, plain.end(), bitmap.begin());
        return true;
    }
};

class Winsock {
    bool ready_ = false;
public:
    Winsock() { WSADATA data{}; ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~Winsock() { if (ready_) WSACleanup(); }
    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
    explicit operator bool() const { return ready_; }
};

class Socket {
    SOCKET value_;
public:
    explicit Socket(SOCKET value) : value_(value) {
        u_long mode = 1;
        if (value_ != INVALID_SOCKET && ioctlsocket(value_, FIONBIO, &mode) != 0) {
            closesocket(value_);
            value_ = INVALID_SOCKET;
        }
    }
    ~Socket() { if (value_ != INVALID_SOCKET) closesocket(value_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    explicit operator bool() const { return value_ != INVALID_SOCKET; }
    SOCKET get() const { return value_; }
    void noDelay() const {
        const int yes = 1;
        setsockopt(value_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof yes);
    }
};

inline bool privateEndpoint(const char* ip, unsigned port, sockaddr_in& address) {
    if (port < 1024 || port > 65535 || InetPtonA(AF_INET, ip, &address.sin_addr) != 1) return false;
    address.sin_family = AF_INET;
    address.sin_port = htons(u_short(port));
    const uint32_t host = ntohl(address.sin_addr.s_addr);
    return (host >> 24) == 10 || (host >> 24) == 127 ||
        (host >> 20) == 0xac1 || (host >> 16) == 0xc0a8 || (host >> 16) == 0xa9fe;
}

inline bool waitSocket(SOCKET socket, bool writing, uint64_t deadline) {
    const uint64_t now = GetTickCount64();
    if (now >= deadline) return false;
    const long ms = long(deadline - now);
    timeval timeout{ms / 1000, (ms % 1000) * 1000};
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    return select(0, writing ? nullptr : &set, writing ? &set : nullptr, nullptr, &timeout) == 1;
}

template<size_t N>
bool transfer(SOCKET socket, std::array<uint8_t, N>& bytes, bool writing, uint64_t timeout) {
    const uint64_t deadline = GetTickCount64() + timeout;
    for (size_t offset = 0; offset < N;) {
        if (!waitSocket(socket, writing, deadline)) return false;
        auto* data = reinterpret_cast<char*>(bytes.data() + offset);
        const int size = writing ? send(socket, data, int(N - offset), 0) : recv(socket, data, int(N - offset), 0);
        if (size == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (size <= 0) return false;
        offset += size_t(size);
    }
    return true;
}

inline bool connectTo(SOCKET socket, const sockaddr_in& address) {
    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0) return true;
    if (WSAGetLastError() != WSAEWOULDBLOCK || !waitSocket(socket, true, GetTickCount64() + 400)) return false;
    int status = 0, size = sizeof status;
    return getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &size) == 0 && status == 0;
}

}  // namespace lan_keyboard::wire
