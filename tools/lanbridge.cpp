// lanbridge.cpp - tiny LAN bridge: serves the ChatGPT prompt in the local network and saves the answer back.
// Build: tools\build_lanbridge.bat      Run: tools\lanbridge.exe [promptfile] [port]
// No dependencies, Winsock only. Answer appended to tools\bridge_reply.txt (+ *_latest.txt).
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "ws2_32.lib")

static std::string gReport;
static const char* gReplyPath = "bridge_reply.txt";
static const char* gLatestPath = "bridge_reply_latest.txt";

static std::string loadFile(const std::string& p) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") || !f) return "(Datei nicht gefunden: " + p + ")";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}
static void appendFile(const std::string& p, const std::string& s) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "ab") || !f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}
static void writeFile(const std::string& p, const std::string& s) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "wb") || !f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}
static std::string urldecode(const std::string& in) {
    std::string o;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') o += ' ';
        else if (in[i] == '%' && i + 2 < in.size()) {
            char t[3] = {in[i + 1], in[i + 2], 0};
            o += (char)strtol(t, nullptr, 16);
            i += 2;
        } else o += in[i];
    }
    return o;
}
static std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}
static void sendAll(SOCKET s, const std::string& body, const char* ctype) {
    char hdr[512];
    std::snprintf(hdr, sizeof hdr,
                  "HTTP/1.1 200 OK\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", ctype,
                  (int)body.size());
    send(s, hdr, (int)std::strlen(hdr), 0);
    if (!body.empty()) send(s, body.data(), (int)body.size(), 0);
}

static std::string page(const std::string& note) {
    std::string h;
    h += "<!doctype html><html><head><meta charset=\"utf-8\"><title>Ickza LAN-Bridge</title>"
         "<style>body{font-family:Segoe UI,Arial;margin:16px;background:#14161a;color:#e6e6e6}"
         "pre{background:#0d0f12;padding:12px;border:1px solid #2a2f37;white-space:pre-wrap;max-height:50vh;overflow:auto}"
         "textarea{width:100%;height:26vh;background:#0d0f12;color:#e6e6e6;border:1px solid #2a2f37;font-family:Consolas,monospace}"
         "button{background:#2d7d46;color:#fff;border:0;padding:10px 18px;font-size:15px;margin-top:8px;cursor:pointer}"
         "a{color:#7cc4ff}</style></head><body>";
    h += "<h2>Ickza LAN-Bridge &mdash; Prompt an ChatGPT</h2>";
    if (!note.empty()) h += "<p style=\"color:#8ee27a\">" + htmlEscape(note) + "</p>";
    h += "<p>1) Text kopieren &rarr; 2) bei ChatGPT am Main-PC einf&uuml;gen &rarr; 3) Antwort unten einf&uuml;gen und speichern.<br>"
         "<a href=\"/raw\">Prompt als Reintext</a> &middot; <a href=\"/replyraw\">gespeicherte Antwort ansehen</a></p>";
    h += "<pre>" + htmlEscape(gReport) + "</pre>";
    h += "<form method=\"POST\" action=\"/reply\"><b>Antwort von ChatGPT:</b><br>"
         "<textarea name=\"answer\" placeholder=\"Antwort hier einfuegen...\"></textarea><br>"
         "<button type=\"submit\">Antwort speichern</button></form>";
    h += "</body></html>";
    return h;
}

int main(int argc, char** argv) {
    const std::string promptFile = argc > 1 ? argv[1] : "..\\docs\\CHATGPT_PROMPT.md";
    const int port = argc > 2 ? atoi(argv[2]) : 8099;
    gReport = loadFile(promptFile);

    WSADATA w{};
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { std::printf("WSAStartup failed\n"); return 1; }
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { std::printf("socket failed\n"); return 1; }
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((u_short)port);
    if (bind(srv, (sockaddr*)&a, sizeof a) != 0) { std::printf("bind auf Port %d fehlgeschlagen\n", port); return 1; }
    listen(srv, 8);

    char host[256] = {};
    gethostname(host, sizeof host - 1);
    std::printf("=== LAN-Bridge laeuft ===\n");
    std::printf("Prompt: %s (%zu Bytes)\n", promptFile.c_str(), gReport.size());
    std::printf("Antwort -> %s\\%s\n", "tools", gReplyPath);
    struct hostent* he = gethostbyname(host);
    if (he)
        for (int i = 0; he->h_addr_list[i]; ++i) {
            in_addr in;
            std::memcpy(&in, he->h_addr_list[i], 4);
            std::printf("  URL am MAIN-PC:  http://%s:%d/\n", inet_ntoa(in), port);
        }
    std::printf("(Firewall beim ersten Start erlauben!)\n");
    std::fflush(stdout);

    for (;;) {
        sockaddr_in cli{};
        int cl = sizeof cli;
        SOCKET c = accept(srv, (sockaddr*)&cli, &cl);
        if (c == INVALID_SOCKET) continue;

        std::string req;
        char buf[4096];
        int n = recv(c, buf, sizeof buf, 0);
        if (n > 0) req.append(buf, n);
        size_t clen = 0;
        const size_t p = req.find("Content-Length:");
        if (p != std::string::npos) clen = (size_t)atoi(req.c_str() + p + 15);
        const size_t hdrEnd = req.find("\r\n\r\n");
        if (hdrEnd != std::string::npos && clen)
            while (req.size() < hdrEnd + 4 + clen) {
                n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, n);
            }

        const bool isPost = req.rfind("POST", 0) == 0;
        if (isPost && req.rfind("POST /reply", 0) == 0 && hdrEnd != std::string::npos) {
            const std::string body = req.substr(hdrEnd + 4);
            std::string answer;
            const size_t k = body.find("answer=");
            if (k != std::string::npos) {
                std::string val = body.substr(k + 7);
                const size_t amp = val.find('&');
                if (amp != std::string::npos) val.resize(amp);
                answer = urldecode(val);
            }
            SYSTEMTIME st{};
            GetLocalTime(&st);
            char head[128];
            std::snprintf(head, sizeof head, "\n\n===== Antwort von ChatGPT  %04d-%02d-%02d %02d:%02d =====\n", st.wYear, st.wMonth, st.wDay,
                          st.wHour, st.wMinute);
            appendFile(gReplyPath, std::string(head) + answer + "\n");
            writeFile(gLatestPath, answer + "\n");
            std::printf("[%02d:%02d] Antwort gespeichert: %zu Zeichen -> %s\n", st.wHour, st.wMinute, answer.size(), gReplyPath);
            std::fflush(stdout);
            sendAll(c, page("Antwort gespeichert - danke! Jetzt an Codex zurueckgeben."), "text/html");
        } else if (req.rfind("GET /raw", 0) == 0) {
            sendAll(c, gReport, "text/plain");
        } else if (req.rfind("GET /replyraw", 0) == 0) {
            sendAll(c, loadFile(gLatestPath), "text/plain");
        } else {
            sendAll(c, page(""), "text/html");
        }
        closesocket(c);
    }
    return 0;
}