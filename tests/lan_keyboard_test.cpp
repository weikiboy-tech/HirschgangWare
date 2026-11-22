#include "../src/lan_keyboard_wire.h"
#include "../src/lan_keyboard.h"
#include "../src/lan_key_outbox.h"
#include "../src/lan_keyboard_keys.h"
#include "../src/clutch.h"
#include "../tools/lan_keyboard_update.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace lan_keyboard::wire;
using dma_keyboard::Bitmap;

static Bitmap eDown() {
    Bitmap bits{};
    bits['E' / 4] = 1u << (2 * ('E' % 4));
    return bits;
}

template<class Condition>
static void until(Condition condition) {
    const auto deadline = GetTickCount64() + 1200;
    while (!condition() && GetTickCount64() < deadline) Sleep(5);
    assert(condition());
}

int main() {
    const std::string hash(64, 'a');
    const auto release = lan_update::parseRelease("12 " + hash + "\r\n");
    assert(release && release->revision == 12 && release->sha256.front() == 0xaa);
    assert(!lan_update::parseRelease("0 " + hash));
    assert(!lan_update::parseRelease("65536 " + hash));
    assert(!lan_update::parseRelease("12.1 " + hash));
    assert(!lan_update::parseRelease("12 " + hash + "\nmore"));
    assert(!lan_update::parseRelease("12 " + hash.substr(1) + "g"));

    Bitmap allowlist{};
    assert(lan_keyboard::parseAllowedKeys("E", allowlist));
    assert(dma_keyboard::down(allowlist, 'E') && !dma_keyboard::down(allowlist, 'Y'));
    assert(lan_keyboard::parseAllowedKeys("E,Y", allowlist));
    assert(dma_keyboard::down(allowlist, 'E') && dma_keyboard::down(allowlist, 'Y'));
    assert(!dma_keyboard::down(allowlist, 'W'));
    assert(lan_keyboard::parseAllowedKeys("All", allowlist));
    for (unsigned vk = 0; vk < 256; ++vk)
        assert(dma_keyboard::down(allowlist, vk) == dma_keyboard::validKey(vk));
    assert(!lan_keyboard::parseAllowedKeys("All,Y", allowlist));
    assert(!lan_keyboard::parseAllowedKeys("A,B,C,D,E,F,G,H,I", allowlist));

    lan_keyboard::Outbox outbox;
    Bitmap queued{};
    assert(!outbox.publish({}, 10));
    assert(outbox.begin(10, 150, queued) && queued == Bitmap{});
    assert(outbox.publish(eDown(), 11));
    assert(!outbox.publish(eDown(), 12)); // Auto-repeat is still held.
    assert(outbox.publish({}, 13));
    assert(outbox.publish(eDown(), 14));
    assert(outbox.publish({}, 15));
    assert(outbox.next(20, 150, queued) && queued == eDown());
    assert(outbox.next(20, 150, queued) && queued == Bitmap{});
    assert(outbox.next(20, 150, queued) && queued == eDown());
    assert(outbox.next(20, 150, queued) && queued == Bitmap{} && !outbox.pending());
    assert(outbox.publish(eDown(), 21));
    assert(!outbox.publish(eDown(), 172));
    assert(!outbox.next(172, 150, queued)); // Old edges never get replayed late.
    assert(outbox.begin(172, 150, queued) && queued == eDown() && !outbox.pending());
    for (unsigned i = 0; i < 65; ++i) assert(outbox.publish(i % 2 ? eDown() : Bitmap{}, 173 + i));
    assert(!outbox.next(239, 150, queued)); // Backlog overflow invalidates the session.

    Key key{}, other{};
    assert(randomBytes(key.data(), key.size()));
    std::string testHex;
    for (const uint8_t byte : key) {
        constexpr char digits[] = "0123456789abcdef";
        testHex.push_back(digits[byte >> 4]);
        testHex.push_back(digits[byte & 0xf]);
    }
    assert(parseKey(testHex, other) && other == key && !parseKey("invalid", other));
    other = key;
    ++other[0];
    Cipher cipher(key), wrongCipher(other);
    assert(cipher && wrongCipher);
    Challenge first{}, second{};
    assert(randomBytes(first.data(), first.size()) && randomBytes(second.data(), second.size()));
    Packet packet{};
    Bitmap bits{};
    assert(cipher.seal(first, 1, eDown(), packet));
    assert(cipher.open(first, 1, packet, bits) && bits == eDown());
    assert(!wrongCipher.open(first, 1, packet, bits));
    assert(!cipher.open(second, 1, packet, bits));
    assert(!cipher.open(first, 2, packet, bits));
    packet.back() ^= 1;
    assert(!cipher.open(first, 1, packet, bits));
    bits = {};
    bits[0] = 1;
    assert(cipher.seal(first, 1, bits, packet) && !cipher.open(first, 1, packet, bits));

    Winsock network;
    assert(network);
    sockaddr_in address{};
    assert(privateEndpoint("127.0.0.1", kDefaultPort, address));
    assert(!privateEndpoint("8.8.8.8", kDefaultPort, address));
    assert(privateEndpoint("127.0.0.1", kDefaultPort, address));
    address.sin_port = 0;
    {
        Socket reservation(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        assert(reservation && bind(reservation.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
        int length = sizeof address;
        assert(getsockname(reservation.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
    }
    char dir[MAX_PATH]{}, path[MAX_PATH]{};
    assert(GetTempPathA(MAX_PATH, dir) && GetTempFileNameA(dir, "hgk", 0, path));
    assert(WritePrivateProfileStringA("lan_keyboard", "listen", "127.0.0.1", path));
    assert(WritePrivateProfileStringA("lan_keyboard", "port", std::to_string(ntohs(address.sin_port)).c_str(), path));
    assert(WritePrivateProfileStringA("lan_keyboard", "key", testHex.c_str(), path));
    lan_keyboard::start(path, nullptr);
    until([] { return std::string(lan_keyboard::status()) == "LAN keyboard: waiting for main PC"; });

    const auto sendState = [&](const Socket& peer, const Challenge& challenge, uint64_t seq, const Bitmap& bitmap) {
        Packet frame{};
        assert(cipher.seal(challenge, seq, bitmap, frame) && transfer(peer.get(), frame, true, 220));
    };
    uint32_t session = 0;
    {
        Socket peer(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        assert(peer && connectTo(peer.get(), address));
        Challenge challenge{};
        assert(transfer(peer.get(), challenge, false, 500));
        assert(!lan_keyboard::sample().pid);
        sendState(peer, challenge, 1, {});
        until([] { return lan_keyboard::sample().pid != 0; });
        session = lan_keyboard::sample().pid;
        sendState(peer, challenge, 2, eDown());
        until([] { return lan_keyboard::sample().bits == eDown(); });
        assert(lan_keyboard::sample().transport == dma_keyboard::Transport::Lan);
        Sleep(55);
        sendState(peer, challenge, 3, eDown());
        assert(lan_keyboard::sample().bits == eDown());
        sendState(peer, challenge, 4, {});
        until([] { return lan_keyboard::sample().bits == Bitmap{}; });
        assert(lan_keyboard::sample().pid == session);
        const auto released = lan_keyboard::sample();
        assert(released.edges['E'].pressed == 1 && released.edges['E'].released == 1);

        // Two quick taps arrive entirely between application reads.
        sendState(peer, challenge, 5, eDown());
        sendState(peer, challenge, 6, {});
        sendState(peer, challenge, 7, eDown());
        sendState(peer, challenge, 8, {});
        until([] { return lan_keyboard::sample().edges['E'].released == 3; });
        const auto taps = lan_keyboard::sample();
        assert(taps.edges['E'].pressed == 3 && taps.bits == Bitmap{});
        const dma_keyboard::Sample pcie{123, {}, dma_keyboard::Transport::Pcie};
        clutch::State hold, toggle, capture, lanOnly;
        hold.configure({true, 'E', clutch::Activation::Hold});
        toggle.configure({true, 'E', clutch::Activation::Toggle});
        lanOnly.configure({true, 'E', clutch::Activation::Toggle});
        hold.update(released, true);
        hold.update(taps, true);
        assert(!hold.active); // Completed taps do not invent a currently held key.
        toggle.update(clutch::mainPcInput(released, pcie, true), true);
        toggle.update(clutch::mainPcInput(taps, pcie, true), true);
        assert(!toggle.active); // Two genuine presses toggle twice.
        capture.beginBinding();
        capture.update(clutch::mainPcInput(released, {}, false), true);
        capture.update(clutch::mainPcInput(taps, {}, false), true);
        assert(capture.boundKey == 'E' && !capture.binding);
        lanOnly.update(clutch::mainPcInput(taps, {}, false), true);
        sendState(peer, challenge, 9, eDown());
        until([] { return lan_keyboard::sample().bits == eDown(); });
        const auto held = lan_keyboard::sample();
        toggle.update(clutch::mainPcInput(held, pcie, true), true);
        lanOnly.update(clutch::mainPcInput(held, {}, false), true);
        assert(toggle.active && lanOnly.active);
        assert(toggle.lastInput().transport == dma_keyboard::Transport::Lan);
        toggle.update(clutch::mainPcInput(held, pcie, true), true);
        assert(toggle.active); // No extra toggle when the same sample is read again.

        sendState(peer, challenge, 10, {});
        until([] { return lan_keyboard::sample().bits == Bitmap{}; });
        capture.beginBinding();
        capture.update(lan_keyboard::sample(), true);
        Bitmap yDown{};
        lan_keyboard::setKey(yDown, 'Y', true);
        sendState(peer, challenge, 11, yDown);
        until([&] { return lan_keyboard::sample().bits == yDown; });
        const auto yHeld = lan_keyboard::sample();
        capture.update(yHeld, true);
        assert(capture.boundKey == 'Y' && !capture.binding);
        assert(yHeld.edges['Y'].pressed == 1 && !dma_keyboard::down(yHeld.bits, 'E'));
        sendState(peer, challenge, 12, {});
        until([] { return lan_keyboard::sample().edges['Y'].released == 1; });
        until([] { return !lan_keyboard::sample().pid; }); // No heartbeat or release.
        toggle.update(clutch::mainPcInput(lan_keyboard::sample(), pcie, true), true);
        assert(!toggle.active && toggle.ready && toggle.lastInput().pid == pcie.pid);
        lanOnly.update(clutch::mainPcInput(lan_keyboard::sample(), held, true), true);
        assert(!lanOnly.active && !lanOnly.ready); // Reject a stale shared LAN sample.
    }
    {
        Socket peer(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        assert(peer && connectTo(peer.get(), address));
        Challenge challenge{};
        assert(transfer(peer.get(), challenge, false, 500));
        sendState(peer, challenge, 1, eDown());
        until([&] { return lan_keyboard::sample().pid != 0 && lan_keyboard::sample().pid != session; });
        assert(!lan_keyboard::sample().edges['E'].pressed && !lan_keyboard::sample().edges['E'].released);
        clutch::State hold;
        hold.configure({true, 'E', clutch::Activation::Hold});
        hold.update(lan_keyboard::sample(), true);
        assert(!hold.active); // First sample held after reconnect is only a baseline.
        sendState(peer, challenge, 2, {});
        until([] { return lan_keyboard::sample().bits == Bitmap{}; });
        hold.update(lan_keyboard::sample(), true);
        sendState(peer, challenge, 3, eDown());
        until([] { return lan_keyboard::sample().bits == eDown(); });
        hold.update(lan_keyboard::sample(), true);
        assert(hold.active);
    }
    until([] { return !lan_keyboard::sample().pid; });
    lan_keyboard::stop();
    DeleteFileA(path);
    puts("PASS authenticated LAN keyboard, press/hold/release, timeout, replay, and reconnect baseline");
}
