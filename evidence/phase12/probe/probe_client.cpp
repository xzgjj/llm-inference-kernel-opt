// phase12 disconnect probe - client
// Usage:
//   probe_client.exe e1 <port>   E1: httplib client, normal GET /slow
//   probe_client.exe e2 <port>   E2: raw socket, send request then close (FIN, no SO_LINGER)
//   probe_client.exe e3 <port>   E3: raw socket, SO_LINGER{1,0} then close (RST)
//   probe_client.exe e4 <port>   E4: httplib client, abort mid /stream after ~2s
//   probe_client.exe ping <port> liveness check GET /ping
#define _WIN32_WINNT 0x0600
#include <httplib.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace {
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

template <class... Args>
void slog(Args&&... args) {
    std::ostringstream o;
    (o << ... << std::forward<Args>(args));
    std::cout << "[" << now_ts() << "] " << o.str() << std::endl;
    std::cout.flush();
}

long ms_elapsed(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - a)
        .count();
}
}  // namespace

#define CLOG(...) slog(__VA_ARGS__)

static int wsa_start() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

// Open a raw loopback socket, send one HTTP request, report client ephemeral
// port. Returns socket handle (caller closes). linger_rst: false -> FIN
// (default close); true -> SO_LINGER{1,0} -> RST.
static SOCKET raw_request(uint16_t port, bool linger_rst, int& client_port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        CLOG("[client] socket() failed WSA=", WSAGetLastError());
        return INVALID_SOCKET;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        CLOG("[client] connect() failed WSA=", WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    sockaddr_in local{};
    int llen = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &llen);
    client_port = ntohs(local.sin_port);
    CLOG("[client] connected; client_ephemeral_port=", client_port,
         " server_port=", port, " linger_rst=", linger_rst ? "true" : "false");

    std::string req = "GET /slow HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    int sent = send(s, req.c_str(), static_cast<int>(req.size()), 0);
    CLOG("[client] send() bytes=", sent, " err=",
         sent == SOCKET_ERROR ? WSAGetLastError() : 0);
    return s;
}

static void run_e1(uint16_t port) {
    CLOG("[client] e1 start: httplib::Client GET /slow");
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(10, 0);
    auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Get("/slow");
    CLOG("[client] e1 elapsed=", ms_elapsed(t0), "ms valid=", static_cast<bool>(r),
         " status=", r ? r->status : -1,
         " body=", r ? r->body : std::string("<no response>"),
         " err=", httplib::to_string(r.error()));
}

static void run_e2e3(uint16_t port, bool rst, const char* tag) {
    CLOG("[client] ", tag, " start: raw socket, ", rst ? "RST" : "FIN",
         " disconnect before response");
    int client_port = -1;
    SOCKET s = raw_request(port, rst, client_port);
    if (s == INVALID_SOCKET) return;

    // Let the server enter the blocking handler before we drop the socket.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    if (rst) {
        linger ling{1, 0};
        int rc = setsockopt(s, SOL_SOCKET, SO_LINGER,
                            reinterpret_cast<const char*>(&ling), sizeof(ling));
        CLOG("[client] ", tag, " setsockopt(SO_LINGER{1,0}) rc=", rc,
             " err=", rc == SOCKET_ERROR ? WSAGetLastError() : 0);
    }
    CLOG("[client] ", tag, " closing socket (", rst ? "RST" : "FIN",
         "); client_ephemeral_port=", client_port);
    int rc = closesocket(s);
    CLOG("[client] ", tag, " closesocket rc=", rc,
         " err=", rc == SOCKET_ERROR ? WSAGetLastError() : 0);
    // Keep the client process alive briefly so the server write attempt can be
    // observed via the external TCP table snapshot.
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    CLOG("[client] ", tag, " done");
}

static void run_e4(uint16_t port) {
    CLOG("[client] e4 start: httplib::Client GET /stream, abort after ~2s");
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(10, 0);
    int chunks = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool aborted = false;
    auto r = cli.Get(
        "/stream",
        [&](const httplib::Response&) { return true; },
        [&](const char*, size_t) {
            chunks++;
            auto el = ms_elapsed(t0);
            if (!aborted && el > 2000) {
                aborted = true;
                CLOG("[client] e4 aborting receiver at chunk #", chunks,
                     " elapsed=", el, "ms");
                return false;
            }
            return true;
        });
    CLOG("[client] e4 elapsed=", ms_elapsed(t0), "ms chunks_received=", chunks,
         " valid=", static_cast<bool>(r), " status=", r ? r->status : -1,
         " err=", httplib::to_string(r.error()));
}

static void run_ping(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(5, 0);
    auto t0 = std::chrono::steady_clock::now();
    auto r = cli.Get("/ping");
    CLOG("[client] ping elapsed=", ms_elapsed(t0), "ms valid=", static_cast<bool>(r),
         " status=", r ? r->status : -1,
         " body=", r ? r->body : std::string("<no response>"));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        CLOG("[client] usage: probe_client <e1|e2|e3|e4|ping> <port>");
        return 2;
    }
    std::string cmd = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
    if (wsa_start() != 0) {
        CLOG("[client] WSAStartup failed");
        return 1;
    }

    if (cmd == "e1") run_e1(port);
    else if (cmd == "e2") run_e2e3(port, false, "e2");
    else if (cmd == "e3") run_e2e3(port, true, "e3");
    else if (cmd == "e4") run_e4(port);
    else if (cmd == "ping") run_ping(port);
    else CLOG("[client] unknown cmd=", cmd);

    WSACleanup();
    return 0;
}
