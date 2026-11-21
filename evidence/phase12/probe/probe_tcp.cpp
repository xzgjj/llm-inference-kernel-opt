// phase12 disconnect probe - E5: five-tuple reverse lookup against the Windows
// TCP table via GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL).
//
// Starts an httplib server on 127.0.0.1:18510 whose /hold handler blocks 15s.
// Opens raw loopback sockets that hold requests, then matches each known
// five-tuple (local_addr=127.0.0.1, local_port=18510, remote_addr=127.0.0.1,
// remote_port=client ephemeral port) against the enumerated table.
//
// Groups:
//   serial     : 10 one-at-a-time connections
//   concurrent : 20 simultaneously held connections
// Metrics: unique match / no match / multiple match, state histogram,
//          single GetExtendedTcpTable call latency (us), post-FIN transition.
#define _WIN32_WINNT 0x0600
#include <httplib.h>

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {
constexpr uint16_t kServerPort = 18510;

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
#define TLOG(...) slog(__VA_ARGS__)

struct TcpRow {
    DWORD state;
    ULONG local_addr;
    ULONG local_port;
    ULONG remote_addr;
    ULONG remote_port;
    DWORD pid;
};

const char* state_name(DWORD s) {
    switch (s) {
        case MIB_TCP_STATE_CLOSED: return "CLOSED";
        case MIB_TCP_STATE_LISTEN: return "LISTEN";
        case MIB_TCP_STATE_SYN_SENT: return "SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD: return "SYN_RCVD";
        case MIB_TCP_STATE_ESTAB: return "ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1: return "FIN_WAIT1";
        case MIB_TCP_STATE_FIN_WAIT2: return "FIN_WAIT2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING: return "CLOSING";
        case MIB_TCP_STATE_LAST_ACK: return "LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT: return "TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
        default: return "UNKNOWN";
    }
}

// One actual table read. Returns rows; elapsed_us receives the syscall cost.
std::vector<TcpRow> enumerate_table(double& elapsed_us) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    std::vector<BYTE> buf(size);
    auto table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());

    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    DWORD rc = GetExtendedTcpTable(table, &size, TRUE, AF_INET,
                                   TCP_TABLE_OWNER_PID_ALL, 0);
    QueryPerformanceCounter(&b);
    elapsed_us = static_cast<double>(b.QuadPart - a.QuadPart) * 1e6 /
                 static_cast<double>(freq.QuadPart);

    std::vector<TcpRow> rows;
    if (rc != NO_ERROR) {
        TLOG("[tcp] GetExtendedTcpTable rc=", rc);
        return rows;
    }
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        const auto& r = table->table[i];
        rows.push_back({r.dwState, r.dwLocalAddr,
                         ntohs(static_cast<u_short>(r.dwLocalPort)),
                         r.dwRemoteAddr,
                         ntohs(static_cast<u_short>(r.dwRemotePort)),
                         r.dwOwningPid});
    }
    return rows;
}

std::vector<TcpRow> match(const std::vector<TcpRow>& rows, uint16_t local_port,
                          uint16_t remote_port) {
    std::vector<TcpRow> hit;
    for (const auto& r : rows) {
        if (r.local_port == local_port && r.remote_port == remote_port &&
            r.local_addr == htonl(INADDR_LOOPBACK) &&
            r.remote_addr == htonl(INADDR_LOOPBACK)) {
            hit.push_back(r);
        }
    }
    return hit;
}

SOCKET hold_connection(uint16_t server_port, int& client_port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
        SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    sockaddr_in local{};
    int llen = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &llen);
    client_port = ntohs(local.sin_port);
    std::string req = "GET /hold HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(server_port) + "\r\n\r\n";
    send(s, req.c_str(), static_cast<int>(req.size()), 0);
    return s;
}

struct Stats {
    int total = 0;
    int unique = 0;
    int none = 0;
    int multiple = 0;
    std::vector<std::string> states;
    std::vector<double> lat_us;
};

void report(const char* name, const Stats& st) {
    TLOG("[tcp] === ", name, " ===");
    TLOG("[tcp] ", name, " total=", st.total, " unique=", st.unique,
         " none=", st.none, " multiple=", st.multiple,
         " match_rate=", st.total ? 100.0 * st.unique / st.total : 0.0, "%",
         " conflict_rate=", st.total ? 100.0 * st.multiple / st.total : 0.0, "%");
    std::ostringstream hist;
    for (const auto& s : st.states) hist << s << " ";
    TLOG("[tcp] ", name, " state histogram: ", hist.str());
    if (!st.lat_us.empty()) {
        double mn = 1e18, mx = 0, sum = 0;
        for (double v : st.lat_us) { mn = std::min(mn, v); mx = std::max(mx, v); sum += v; }
        TLOG("[tcp] ", name, " enum latency us: min=", mn, " avg=", sum / st.lat_us.size(),
             " max=", mx, " samples=", st.lat_us.size());
    }
}
}  // namespace

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    httplib::Server svr;
    svr.Get("/hold", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        res.set_content("hold-ok", "text/plain");
    });
    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });
    std::thread server_thread([&] { svr.listen("127.0.0.1", kServerPort); });
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Warm-up enumeration (size query + real read).
    double warm = 0;
    auto warm_rows = enumerate_table(warm);
    TLOG("[tcp] warm-up table rows=", warm_rows.size(), " first read us=", warm);

    // ---------------- serial group: 10 one-at-a-time ----------------
    Stats serial;
    constexpr int kTrials = 10;
    for (int i = 0; i < kTrials; i++) {
        int cport = -1;
        SOCKET s = hold_connection(kServerPort, cport);
        if (s == INVALID_SOCKET) { TLOG("[tcp] serial connect failed i=", i); continue; }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        // 20 timed enumerations for latency.
        for (int k = 0; k < 20; k++) {
            double us = 0;
            auto rows = enumerate_table(us);
            serial.lat_us.push_back(us);
            if (k == 0) {
                auto hit = match(rows, kServerPort, static_cast<uint16_t>(cport));
                serial.total++;
                if (hit.size() == 1) {
                    serial.unique++;
                    serial.states.push_back(state_name(hit[0].state));
                } else if (hit.empty()) {
                    serial.none++;
                    serial.states.push_back("GONE");
                } else {
                    serial.multiple++;
                    serial.states.push_back("MULTI:" + std::to_string(hit.size()));
                }
                TLOG("[tcp] serial trial=", i, " client_port=", cport,
                     " hits=", hit.size(),
                     " state=", hit.empty() ? "GONE" : state_name(hit[0].state));
            }
        }
        closesocket(s);

        // Post-FIN transition observation: 5 polls @ 50ms.
        for (int p = 0; p < 5; p++) {
            double us = 0;
            auto rows = enumerate_table(us);
            auto hit = match(rows, kServerPort, static_cast<uint16_t>(cport));
            TLOG("[tcp] serial post-FIN poll=", p, " client_port=", cport,
                 " hits=", hit.size(),
                 " state=", hit.empty() ? "GONE" : state_name(hit[0].state));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    report("serial", serial);

    // ---------------- concurrent group: 20 held connections ----------------
    Stats concurrent;
    constexpr int kConns = 20;
    std::vector<SOCKET> socks;
    std::vector<int> cports;
    for (int i = 0; i < kConns; i++) {
        int cport = -1;
        SOCKET s = hold_connection(kServerPort, cport);
        if (s != INVALID_SOCKET) { socks.push_back(s); cports.push_back(cport); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    TLOG("[tcp] concurrent established=", socks.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Latency sample: 20 enumerations under concurrency.
    for (int k = 0; k < 20; k++) {
        double us = 0;
        auto rows = enumerate_table(us);
        concurrent.lat_us.push_back(us);
        if (k == 0) {
            for (int i = 0; i < static_cast<int>(cports.size()); i++) {
                auto hit = match(rows, kServerPort,
                                 static_cast<uint16_t>(cports[i]));
                concurrent.total++;
                if (hit.size() == 1) {
                    concurrent.unique++;
                    concurrent.states.push_back(state_name(hit[0].state));
                } else if (hit.empty()) {
                    concurrent.none++;
                    concurrent.states.push_back("GONE");
                } else {
                    concurrent.multiple++;
                    concurrent.states.push_back("MULTI:" + std::to_string(hit.size()));
                }
                TLOG("[tcp] concurrent conn=", i, " client_port=", cports[i],
                     " hits=", hit.size(),
                     " state=", hit.empty() ? "GONE" : state_name(hit[0].state));
            }
        }
    }
    report("concurrent", concurrent);

    for (auto s : socks) closesocket(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    svr.stop();
    server_thread.join();
    WSACleanup();
    TLOG("[tcp] done");
    return 0;
}
