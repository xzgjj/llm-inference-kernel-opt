// phase12 E7 probe: httplib v0.18.5 Request::is_connection_closed, measured in
// a NON-STREAMING blocking handler. Read-only experiment; product code untouched.
//
// Modes:
//   probe_isconnclosed.exe server <port>  : /slow polls is_connection_closed
//                                           every 200ms; /ping liveness
//   probe_isconnclosed.exe fin    <port>  : raw socket, FIN close (no SO_LINGER)
//   probe_isconnclosed.exe rst    <port>  : raw socket, SO_LINGER{1,0} -> RST
//   probe_isconnclosed.exe normal <port>  : httplib client, full normal GET
//   probe_isconnclosed.exe ping   <port>  : liveness GET /ping
#define _WIN32_WINNT 0x0600
#include <httplib.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace {
std::mutex g_mtx;

std::string now_ts() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    struct tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min,
             tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

long long now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

template <class... Args>
void slog(Args&&... args) {
    std::ostringstream o;
    (o << ... << std::forward<Args>(args));
    std::lock_guard<std::mutex> lk(g_mtx);
    std::cout << "[" << now_ts() << "] " << o.str() << " (epoch=" << now_epoch_ms()
              << ")" << std::endl;
    std::cout.flush();
}

long ms_elapsed(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - a)
        .count();
}
}  // namespace

#define SLOG(...) slog(__VA_ARGS__)

static void run_server(uint16_t port) {
    httplib::Server svr;

    svr.Get("/slow", [](const httplib::Request& req, httplib::Response& res) {
        SLOG("[srv] /slow enter remote=", req.remote_addr, ":", req.remote_port,
             " local=", req.local_addr, ":", req.local_port);
        auto t0 = std::chrono::steady_clock::now();
        bool closed_seen = false;
        for (int i = 1; i <= 15; i++) {
            bool closed = req.is_connection_closed();
            SLOG("[srv] poll #", i, " elapsed=", ms_elapsed(t0),
                 "ms is_connection_closed=", closed ? "true" : "false");
            if (closed) {
                closed_seen = true;
                SLOG("[srv] DETECTED closed at poll #", i, " elapsed=",
                     ms_elapsed(t0), "ms");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        SLOG("[srv] /slow leaving loop closed_seen=", closed_seen, " elapsed=",
             ms_elapsed(t0), "ms");
        res.set_content("slow-ok", "text/plain");
        SLOG("[srv] /slow set_content returned; handler exit");
    });

    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    SLOG("[srv] listening on 127.0.0.1:", port);
    svr.listen("127.0.0.1", port);
}

static void run_raw(uint16_t port, bool rst, const char* tag) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        SLOG("[cli] ", tag, " socket() failed WSA=", WSAGetLastError());
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        SLOG("[cli] ", tag, " connect() failed WSA=", WSAGetLastError());
        closesocket(s);
        return;
    }
    sockaddr_in local{};
    int llen = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &llen);
    int cport = ntohs(local.sin_port);
    SLOG("[cli] ", tag, " connected; client_ephemeral_port=", cport,
         " server_port=", port, " rst=", rst ? "true" : "false");

    std::string req = "GET /slow HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    int sent = send(s, req.c_str(), static_cast<int>(req.size()), 0);
    SLOG("[cli] ", tag, " send() bytes=", sent);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (rst) {
        linger ling{1, 0};
        int rc = setsockopt(s, SOL_SOCKET, SO_LINGER,
                            reinterpret_cast<const char*>(&ling), sizeof(ling));
        SLOG("[cli] ", tag, " setsockopt(SO_LINGER{1,0}) rc=", rc);
    }
    long long close_ms = now_epoch_ms();
    int rc = closesocket(s);
    SLOG("[cli] ", tag, " closesocket rc=", rc, " close_epoch_ms=", close_ms,
         " (", rst ? "RST" : "FIN", ")");

    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    WSACleanup();
}

static void run_normal(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(10, 0);
    auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Get("/slow");
    SLOG("[cli] normal elapsed=", ms_elapsed(t0), "ms valid=",
         static_cast<bool>(r), " status=", r ? r->status : -1,
         " body=", r ? r->body : std::string("<none>"),
         " err=", httplib::to_string(r.error()));
}

static void run_ping(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);
    auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Get("/ping");
    SLOG("[cli] ping elapsed=", ms_elapsed(t0), "ms valid=",
         static_cast<bool>(r), " status=", r ? r->status : -1,
         " body=", r ? r->body : std::string("<none>"));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        SLOG("[cli] usage: probe_isconnclosed <server|fin|rst|normal|ping> <port>");
        return 2;
    }
    std::string cmd = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));

    if (cmd == "server") run_server(port);
    else if (cmd == "fin") run_raw(port, false, "fin");
    else if (cmd == "rst") run_raw(port, true, "rst");
    else if (cmd == "normal") run_normal(port);
    else if (cmd == "ping") run_ping(port);
    else SLOG("[cli] unknown cmd=", cmd);
    return 0;
}
