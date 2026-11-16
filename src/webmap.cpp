#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include "webmap_state.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <locale>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace webmap {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr size_t kHeaders = 8192;
constexpr size_t kChunk = 64 * 1024;
constexpr size_t kClients = 16;
constexpr uint64_t kImageLimit = 64 * 1024 * 1024;

struct Settings {
    Options options;
    fs::path root;
};

struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t remaining = 0;
    File() = default;
    File(File&& other) noexcept : handle(other.handle), remaining(other.remaining) {
        other.handle = INVALID_HANDLE_VALUE;
    }
    File& operator=(File&& other) noexcept {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = other.handle;
        remaining = other.remaining;
        other.handle = INVALID_HANDLE_VALUE;
        return *this;
    }
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
};

bool plainDirectory(const fs::path& path) {
    const DWORD flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) &&
           !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
}

File openFile(const fs::path& dir, const wchar_t* name, uint64_t maxSize) {
    File file;
    if (!plainDirectory(dir)) return file;
    file.handle = CreateFileW((dir / name).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (!file) return file;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
    file.remaining = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (!file.remaining || file.remaining > maxSize) return {};
    return file;
}

std::string readSmall(const fs::path& dir, const std::wstring& name, DWORD cap) {
    auto file = openFile(dir, name.c_str(), cap);
    if (!file) return {};
    std::string bytes(size_t(file.remaining), '\0');
    DWORD read = 0;
    if (!ReadFile(file.handle, bytes.data(), DWORD(bytes.size()), &read, nullptr) || read != bytes.size()) return {};
    return bytes;
}

bool validId(std::string_view id) {
    return !id.empty() && id.size() <= 48 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::wstring wideId(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

bool publicHostValid(std::string_view s) {
    if (s.size() > 253 || s.find('.') == s.npos || s.front() == '.' || s.back() == '.') return false;
    size_t next = 0;
    while (next < s.size()) {
        const size_t dot = s.find('.', next);
        const auto label = s.substr(next, dot == s.npos ? s.size() - next : dot - next);
        if (!validId(label) || label.find('_') != label.npos || label.front() == '-' || label.back() == '-') return false;
        if (dot == s.npos) break;
        next = dot + 1;
    }
    return true;
}

std::string json(std::string_view s) {
    // Reject malformed UTF-8 instead of forwarding invalid JSON or platform-encoded bytes.
    if (!s.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0))
        return "\"?\"";
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (c < 32) out += ' ';
        else out += char(c);
    }
    out += '"';
    return out;
}

struct Map {
    std::string id;
    std::wstring file;
    float size = 0;
};
using Maps = std::vector<Map>;

Maps loadMaps(const fs::path& dir) {
    Maps result;
    if (!plainDirectory(dir)) return result;
    std::error_code ec;
    unsigned scanned = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end && ++scanned <= 512; it.increment(ec)) {
        const auto ext = it->path().extension().wstring();
        if (ext != L".png" && ext != L".jpg") continue;
        const auto stem = it->path().stem().wstring();
        if (std::any_of(stem.begin(), stem.end(), [](wchar_t c) { return c > 127; })) continue;
        std::string id;
        id.reserve(stem.size());
        for (const wchar_t c : stem) id += static_cast<char>(c);
        const bool topo = id.size() > 5 && id.substr(id.size() - 5) == "_topo";
        if (topo) id.resize(id.size() - 5);
        if (!validId(id) || !openFile(dir, it->path().filename().c_str(), kImageLimit)) continue;

        auto existing = std::find_if(result.begin(), result.end(), [&](const Map& m) { return m.id == id; });
        if (existing != result.end()) {
            const bool existingTopo = existing->file.find(L"_topo.") != std::wstring::npos;
            if ((existingTopo && !topo) || (existingTopo == topo && ext == L".png"))
                existing->file = it->path().filename().wstring();
            continue;
        }
        if (result.size() >= 64) continue;
        std::istringstream meta(readSmall(dir, wideId(id) + L".size", 128));
        meta.imbue(std::locale::classic());
        float size = 0;
        if (!(meta >> size) || !std::isfinite(size) || size < 256 || size > 100000) continue;
        result.push_back({id, it->path().filename().wstring(), size});
    }
    std::sort(result.begin(), result.end(), [](const Map& a, const Map& b) { return a.id < b.id; });
    return result;
}

bool inside(float x, float z, float size) { return x >= 0 && z >= 0 && x <= size && z <= size; }

struct Layer {
    Release release;
    radar::Icon icon;
    const char* label;
};

constexpr Layer kLayers[] = {
    {Players, radar::Icon::Survivor, "players"},
    {Infected, radar::Icon::Infected, "infected"},
    {Animals, radar::Icon::Animal, "animals"},
    {Vehicles, radar::Icon::Vehicle, "vehicles"},
    {Loot, radar::Icon::Loot, "loot"},
};

void appendHeading(std::ostream& out, float x, float z) {
    if (std::isfinite(x) && std::isfinite(z) && std::hypot(x, z) > 0.001f)
        out << ",\"heading\":" << std::atan2(x, z) * (180.0 / 3.14159265358979323846);
}

void appendPlayerName(std::ostream& out, const Options& opt, const std::string& name) {
    if (opt.shares(PlayerNames) && !name.empty() && name.size() <= 80)
        out << ",\"name\":" << json(name);
}

const std::string& trackingSession() {
    static const auto id = [] {
        unsigned char bytes[8]{};
        if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return std::string{};
        constexpr char hex[] = "0123456789abcdef";
        std::string token;
        for (unsigned char byte : bytes) {
            token += hex[byte >> 4];
            token += hex[byte & 15];
        }
        return token;
    }();
    return id;
}

std::string packet(const Options& opt, const std::shared_ptr<const Snapshot>& snap) {
    const bool live = opt.shares(CurrentMap) && snap && Clock::now() - snap->captured < 1500ms;
    const std::string map = live && opt.shares(CurrentMap) && validId(snap->map) ? snap->map : "";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"v\":1,\"live\":" << (live ? "true" : "false") << ",\"map\":"
        << (map.empty() ? "null" : json(map)) << ",\"me\":";
    if (!map.empty() && opt.shares(Own) && snap->frame.ok &&
        inside(snap->frame.meX, snap->frame.meZ, snap->frame.mapSize)) {
        const auto& fr = snap->frame;
        out << "{\"x\":" << fr.meX << ",\"z\":" << fr.meZ;
        if (opt.shares(Heading)) appendHeading(out, fr.fwdX, fr.fwdZ);
        appendPlayerName(out, opt, fr.meName);
        out << '}';
    } else {
        out << "null";
    }
    out << ",\"layers\":{";
    bool firstLayer = true;
    for (const auto& layer : kLayers) {
        if (map.empty() || !opt.shares(layer.release)) continue;
        if (!firstLayer) out << ',';
        firstLayer = false;
        out << json(layer.label) << ":[";
        unsigned count = 0;
        for (const auto& d : snap->frame.dots) {
            if (d.icon != layer.icon || !inside(d.x, d.z, snap->frame.mapSize)) continue;
            if (count++) out << ',';
            out << "{\"x\":" << d.x << ",\"z\":" << d.z;
            if (d.icon == radar::Icon::Loot) out << ",\"category\":" << d.cat;
            if (d.icon == radar::Icon::Survivor) {
                appendHeading(out, d.fwdX, d.fwdZ);
                appendPlayerName(out, opt, d.name);
                if (d.webFriend) out << ",\"friend\":true";
                if (d.webTrackId && !trackingSession().empty())
                    out << ",\"id\":\"" << trackingSession() << '-' << d.webTrackId << '"';
            }
            out << '}';
            if (count == 512) break;
        }
        out << ']';
    }
    out << "}}";
    return out.str();
}

std::string mapList(const Maps& maps) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '[';
    for (size_t i = 0; i < maps.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":" << json(maps[i].id) << ",\"size\":" << maps[i].size << '}';
    }
    out << ']';
    return out.str();
}

std::string places(const fs::path& dir, const Map& map) {
    std::istringstream in(readSmall(dir, wideId(map.id) + L"_names.txt", 128 * 1024));
    in.imbue(std::locale::classic());
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '[';
    std::string line;
    unsigned count = 0;
    while (count < 512 && std::getline(in, line)) {
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        radar::Place place;
        std::string type;
        if (!(row >> place.x >> place.z >> type) || !inside(place.x, place.z, map.size)) continue;
        std::getline(row >> std::ws, place.name);
        if (!place.name.empty() && place.name.back() == '\r') place.name.pop_back();
        if (place.name.empty() || place.name.size() > 128) continue;
        if (count++) out << ',';
        out << "{\"x\":" << place.x << ",\"z\":" << place.z << ",\"name\":" << json(place.name) << '}';
    }
    out << ']';
    return out.str();
}

struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::string rx, tx;
    size_t sent = 0;
    bool websocket = false, closeAfter = false, sendingFile = false;
    Clock::time_point opened = Clock::now(), progress = opened, nextPush = opened;
    File file;
};

void sendData(Client& c, std::string bytes) {
    c.tx = std::move(bytes);
    c.sent = 0;
    c.progress = Clock::now();
}

void response(Client& c, const char* status, const char* type, std::string body, uint64_t size = 0) {
    c.closeAfter = true;
    std::string headers = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + std::to_string(size ? size : body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
        "\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self';"
        " script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self' ws: wss:;"
        " frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n\r\n";
    sendData(c, std::move(headers) + body);
}

std::string frame(unsigned opcode, std::string_view text) {
    std::string out(1, char(0x80 | opcode));
    const size_t len = text.size();
    if (len < 126) out += char(len);
    else if (len <= 65535) {
        out += char(126);
        out += char(len >> 8);
        out += char(len);
    } else {
        out += char(127);
        for (int i = 56; i >= 0; i -= 8) out += char(uint64_t(len) >> i);
    }
    out += text;
    return out;
}

bool websocketInput(Client& c) {
    if (c.rx.size() < 2) return true;
    const auto a = uint8_t(c.rx[0]), b = uint8_t(c.rx[1]);
    const unsigned op = a & 15, length = b & 127;
    const bool valid = (a & 0xf0) == 0x80 && (b & 0x80) && length <= 125 &&
                       (op == 8 || op == 9 || op == 10) && (op != 8 || length != 1);
    if (!valid) {
        if (!c.tx.empty()) return false;
        sendData(c, frame(8, std::string_view("\x03\xea", 2)));
        c.closeAfter = true;
        return true;
    }
    if (c.rx.size() < 6 + length || !c.tx.empty()) return true;
    std::string payload = c.rx.substr(6, length);
    for (size_t i = 0; i < length; ++i) payload[i] ^= c.rx[2 + i % 4];
    c.rx.erase(0, 6 + length);
    if (op == 9) sendData(c, frame(10, payload));
    if (op == 8) {
        sendData(c, frame(8, {}));
        c.closeAfter = true;
    }
    return true;
}

std::string acceptKey(const std::string& key) {
    if (key.size() != 24) return {};
    DWORD decoded = 0;
    if (!CryptStringToBinaryA(key.c_str(), DWORD(key.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              nullptr, &decoded, nullptr, nullptr) || decoded != 16) return {};
    const std::string nonce = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) return {};
    BYTE digest[20]{};
    const auto rc = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data())),
                               ULONG(nonce.size()), digest, sizeof digest);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (rc < 0) return {};
    char out[32]{};
    DWORD len = sizeof out;
    return CryptBinaryToStringA(digest, sizeof digest, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &len)
        ? std::string(out) : std::string{};
}

using Headers = std::unordered_map<std::string, std::string>;

std::string_view trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == s.npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

bool token(std::string_view s, std::string_view name) {
    while (!s.empty()) {
        const auto n = s.find(',');
        if (lower(trim(s.substr(0, n))) == name) return true;
        if (n == s.npos) break;
        s.remove_prefix(n + 1);
    }
    return false;
}

bool request(Client& c, const Settings& setup, const Maps& maps) {
    const auto end = c.rx.find("\r\n\r\n");
    if (end == c.rx.npos) return c.rx.size() < kHeaders;
    std::string method, path, version, extra, line;
    auto bad = [&] { response(c, "400 Bad Request", "text/plain", "Invalid request"); return true; };
    std::istringstream input(c.rx.substr(0, end + 2));
    std::getline(input, line);
    if (!line.empty()) line.pop_back();
    std::istringstream first(line);
    if (!(first >> method >> path >> version) || first >> extra || version != "HTTP/1.1" || end > kHeaders)
        return bad();
    Headers headers;
    while (std::getline(input, line)) {
        if (line.empty() || line.back() != '\r') return bad();
        line.pop_back();
        const auto colon = line.find(':');
        if (colon == line.npos || !colon || headers.size() >= 48) return bad();
        const auto name = lower(std::string_view(line).substr(0, colon));
        if (!headers.emplace(name, trim(std::string_view(line).substr(colon + 1))).second) return bad();
    }
    c.rx.erase(0, end + 4);
    if (method != "GET") {
        response(c, "405 Method Not Allowed", "text/plain", "GET only");
        return true;
    }
    if (headers.count("transfer-encoding") || (headers.count("content-length") && headers["content-length"] != "0"))
        return bad();

    const auto& opt = setup.options;
    const std::string local = "127.0.0.1:" + std::to_string(opt.port);
    const std::string host = lower(headers["host"]);
    const bool publicRoute = !opt.publicHost.empty() && host == opt.publicHost &&
                             !headers["cf-access-jwt-assertion"].empty();
    const std::string origin = host == local ? "http://" + local : "https://" + opt.publicHost;
    if ((host != local && !publicRoute) || headers["sec-fetch-site"] == "cross-site" ||
        (!headers["origin"].empty() && headers["origin"] != origin)) {
        response(c, "403 Forbidden", "text/plain", "Origin or host denied");
        return true;
    }
    if (path == "/ws") {
        const auto key = acceptKey(headers["sec-websocket-key"]);
        if (key.empty() || lower(headers["upgrade"]) != "websocket" ||
            !token(headers["connection"], "upgrade") || headers["sec-websocket-version"] != "13" ||
            headers["origin"] != origin) {
            response(c, "403 Forbidden", "text/plain", "WebSocket upgrade denied");
            return true;
        }
        c.websocket = true;
        sendData(c, "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: " +
                    key + "\r\n\r\n");
        return true;
    }
    if (path == "/api/status") {
        response(c, "200 OK", "application/json", "{\"running\":true,\"hz\":" + std::to_string(opt.hz) + "}");
        return true;
    }
    if (path == "/api/maps") {
        response(c, "200 OK", "application/json", mapList(maps));
        return true;
    }
    const auto dir = setup.root / "maps";
    const bool image = path.rfind("/maps/", 0) == 0;
    const bool place = path.rfind("/api/places/", 0) == 0;
    if (image || place) {
        const auto id = path.substr(image ? 6 : 12);
        if (!validId(id)) return bad();
        const auto map = std::find_if(maps.begin(), maps.end(), [&](const Map& m) { return m.id == id; });
        if (map != maps.end()) {
            if (place) {
                response(c, "200 OK", "application/json", places(dir, *map));
                return true;
            }
            c.file = openFile(dir, map->file.c_str(), kImageLimit);
            if (c.file) {
                c.sendingFile = true;
                const auto type = fs::path(map->file).extension() == L".png" ? "image/png" : "image/jpeg";
                response(c, "200 OK", type, {}, c.file.remaining);
                return true;
            }
        }
    } else {
        struct Asset { const char* route; const wchar_t* file; const char* type; };
        constexpr Asset assets[] = {
            {"/", L"index.html", "text/html; charset=utf-8"},
            {"/hirschgang-dont-press-me", L"index.html", "text/html; charset=utf-8"},
            {"/hirschgang-logo.png", L"hirschgang-logo.png", "image/png"},
            {"/app.js", L"app.js", "text/javascript; charset=utf-8"},
            {"/map.js", L"map.js", "text/javascript; charset=utf-8"},
            {"/live.js", L"live.js", "text/javascript; charset=utf-8"},
            {"/style.css", L"style.css", "text/css; charset=utf-8"},
        };
        for (const auto& asset : assets) {
            if (path != asset.route) continue;
            auto body = readSmall(setup.root / "web", asset.file, 256 * 1024);
            if (!body.empty()) {
                response(c, "200 OK", asset.type, std::move(body));
                return true;
            }
        }
    }
    response(c, "404 Not Found", "text/plain", "Not found");
    return true;
}

bool tick(Client& c, const Settings& setup, const Maps& maps, std::string_view latest, Clock::time_point now) {
    if ((!c.websocket && now - c.opened > (c.sendingFile ? 120s : 6s)) ||
        (!c.tx.empty() && now - c.progress > 8s)) return false;
    if (!c.closeAfter) {
        char bytes[2048];
        const int n = recv(c.socket, bytes, sizeof bytes, 0);
        if (n == 0 || (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) return false;
        if (n > 0) c.rx.append(bytes, size_t(n));
        if (c.rx.size() > kHeaders || (!c.websocket && !request(c, setup, maps))) return false;
        if (c.websocket && !websocketInput(c)) return false;
    }
    if (c.tx.empty() && c.websocket && !c.closeAfter && now >= c.nextPush) {
        sendData(c, frame(1, latest));
        c.nextPush = now + std::chrono::milliseconds(1000 / setup.options.hz);
    }
    if (c.tx.empty() && c.sendingFile && c.file.remaining) {
        std::string bytes(size_t(std::min<uint64_t>(c.file.remaining, kChunk)), '\0');
        DWORD n = 0;
        if (!ReadFile(c.file.handle, bytes.data(), DWORD(bytes.size()), &n, nullptr) || n == 0) return false;
        bytes.resize(n);
        c.file.remaining -= n;
        sendData(c, std::move(bytes));
    }
    if (!c.tx.empty()) {
        const int n = send(c.socket, c.tx.data() + c.sent, int(c.tx.size() - c.sent), 0);
        if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) return false;
        if (n > 0) {
            c.sent += size_t(n);
            c.progress = now;
            if (c.sent == c.tx.size()) c.tx.clear();
        }
    }
    return !(c.closeAfter && c.tx.empty() && !c.file.remaining);
}

SOCKET listenOn(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    BOOL exclusive = TRUE;
    u_long nonblock = 1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u_short(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive) ||
        ioctlsocket(s, FIONBIO, &nonblock) || bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) ||
        listen(s, int(kClients))) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

class Server {
public:
    ~Server() { stop(); }

    void configure(Options options, const std::string& directory) {
        options.port = std::clamp(options.port, 1024, 65535);
        options.hz = std::clamp(options.hz, 1, 20);
        options.publicHost = lower(options.publicHost);
        if (!publicHostValid(options.publicHost)) options.publicHost.clear();
        auto next = std::make_shared<const Settings>(Settings{std::move(options), fs::path(directory)});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (settings_ && settings_->options.enabled == next->options.enabled &&
                settings_->options.port == next->options.port && settings_->options.hz == next->options.hz &&
                settings_->options.release == next->options.release &&
                settings_->options.publicHost == next->options.publicHost && settings_->root == next->root) return;
            settings_ = std::move(next);
            snapshot_.reset();
            status_ = settings_->options.enabled ? "Starting" : "Off";
            if (settings_->options.enabled && !worker_.joinable()) worker_ = std::thread([this] { run(); });
        }
        cv_.notify_one();
    }

    void publish(std::shared_ptr<const Snapshot> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = std::move(value);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) {
            CancelSynchronousIo(worker_.native_handle());
            worker_.join();
        }
    }

    std::string status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

private:
    void setStatus(const std::shared_ptr<const Settings>& setup, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settings_ == setup) status_ = std::move(message);
    }

    void run() {
        WSADATA data{};
        const bool winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        SOCKET listener = INVALID_SOCKET;
        std::vector<Client> clients;
        auto closeAll = [&] {
            for (auto& c : clients) closesocket(c.socket);
            clients.clear();
            if (listener != INVALID_SOCKET) closesocket(listener);
            listener = INVALID_SOCKET;
        };
        std::shared_ptr<const Settings> current;
        Maps maps;
        Clock::time_point catalogAt{}, packetAt{}, retryAt{};
        std::string latest;
        for (;;) {
            std::shared_ptr<const Settings> setup;
            std::shared_ptr<const Snapshot> snap;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, listener == INVALID_SOCKET ? 100ms : 8ms);
                if (stopping_) break;
                setup = settings_;
                snap = snapshot_;
            }
            if (current != setup) {
                // Closing established clients also drops any pending data under the old release settings.
                closeAll();
                current = setup;
                retryAt = catalogAt = packetAt = {};
            }
            if (!setup || !setup->options.enabled) continue;
            auto now = Clock::now();
            if (listener == INVALID_SOCKET && now >= retryAt) {
                retryAt = now + 2s;
                if (winsock) listener = listenOn(setup->options.port);
                setStatus(setup, listener != INVALID_SOCKET ? "Listening on 127.0.0.1:" + std::to_string(setup->options.port)
                                                           : "Cannot bind Webmap port");
            }
            if (listener == INVALID_SOCKET) continue;
            if (now >= catalogAt) {
                maps = loadMaps(setup->root / "maps");
                catalogAt = now + 3s;
            }
            if (now >= packetAt) {
                latest = packet(setup->options, snap);
                packetAt = now + std::chrono::milliseconds(1000 / setup->options.hz);
            }
            for (unsigned i = 0; i < 4; ++i) {
                const SOCKET socket = accept(listener, nullptr, nullptr);
                if (socket == INVALID_SOCKET) break;
                u_long nonblock = 1;
                if (clients.size() >= kClients || ioctlsocket(socket, FIONBIO, &nonblock)) closesocket(socket);
                else {
                    clients.emplace_back();
                    clients.back().socket = socket;
                }
            }
            for (size_t i = 0; i < clients.size();) {
                if (tick(clients[i], *setup, maps, latest, now)) ++i;
                else {
                    closesocket(clients[i].socket);
                    clients.erase(clients.begin() + i);
                }
            }
        }
        closeAll();
        if (winsock) WSACleanup();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<const Settings> settings_;
    std::shared_ptr<const Snapshot> snapshot_;
    std::thread worker_;
    bool stopping_ = false;
    std::string status_ = "Off";
};

Server& server() { static Server instance; return instance; }

}

void configure(const Options& options, const std::string& root) { server().configure(options, root); }
void publish(const std::string& mapId, radar::Frame frame) {
    server().publish(std::make_shared<const Snapshot>(Snapshot{mapId, std::move(frame), Clock::now()}));
}
void clear() { server().publish({}); }
void shutdown() { server().stop(); }
std::string status() { return server().status(); }

}
