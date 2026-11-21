// phase12 disconnect probe - server (read-only experiment, product code untouched)
// Routes:
//   /slow  : non-streaming handler, sleeps 3s then set_content (E1/E2/E3)
//   /stream: chunked provider, callback every 200ms, reports sink.is_writable() (E4)
//   /ping  : liveness check after broken connections
#define _WIN32_WINNT 0x0600
#include <httplib.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

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

template <class... Args>
void slog(Args&&... args) {
    std::ostringstream o;
    (o << ... << std::forward<Args>(args));
    std::lock_guard<std::mutex> lk(g_mtx);
    std::cout << "[" << now_ts() << "] " << o.str() << std::endl;
    std::cout.flush();
}

long ms_elapsed(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - a)
        .count();
}
}  // namespace

// Forward the variadic pack to the variadic template (macro commas stay
// argument separators here; they must NOT be turned into stream inserts).
#define SLOG(...) slog(__VA_ARGS__)

int main(int argc, char** argv) {
    uint16_t port = 18501;
    if (argc > 1) { port = static_cast<uint16_t>(std::stoi(argv[1])); }

    httplib::Server svr;

    // E1 / E2 / E3: non-streaming handler that blocks for ~3s.
    svr.Get("/slow", [](const httplib::Request& req, httplib::Response& res) {
        SLOG("[server] /slow enter remote=", req.remote_addr, ":", req.remote_port,
             " local=", req.local_addr, ":", req.local_port,
             " method=", req.method, " target=", req.target,
             " headers=", req.headers.size(), " body=", req.body.size());

        auto t0 = std::chrono::steady_clock::now();
        // Simulate a long compute task. The handler has no socket fd and no
        // disconnect callback; this sleep is the observation window.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        auto t1 = std::chrono::steady_clock::now();
        SLOG("[server] /slow woke after ", ms_elapsed(t0),
             "ms (sleep measured ", ms_elapsed(t0),
             "ms); about to call set_content");

        res.set_content("slow-ok", "text/plain");
        auto t2 = std::chrono::steady_clock::now();
        SLOG("[server] /slow set_content returned after ", ms_elapsed(t1),
             "ms; handler exit");
    });

    // E4: chunked provider control truth.
    svr.Get("/stream", [](const httplib::Request& req, httplib::Response& res) {
        SLOG("[server] /stream enter remote=", req.remote_addr, ":", req.remote_port,
             " local=", req.local_addr, ":", req.local_port);
        int cb = 0;
        auto t0 = std::chrono::steady_clock::now();
        res.set_chunked_content_provider(
            "text/plain",
            [&](size_t offset, httplib::DataSink& sink) -> bool {
                // Poll inside the provider so we can observe is_writable()
                // flipping false after the client drops mid-stream.
                while (true) {
                    cb++;
                    bool writable = static_cast<bool>(sink.is_writable());
                    SLOG("[server] /stream callback #", cb, " offset=", offset,
                         " elapsed=", ms_elapsed(t0), "ms is_writable=",
                         writable ? "true" : "false");
                    if (!writable) {
                        SLOG("[server] /stream writable=false at callback #", cb,
                             " elapsed=", ms_elapsed(t0), "ms; abort provider");
                        return false;
                    }
                    std::string chunk = "chunk-" + std::to_string(cb) + "\n";
                    bool ok = sink.write(chunk.data(), chunk.size());
                    SLOG("[server] /stream sink.write ok=", ok);
                    if (cb >= 30) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            },
            [](bool success) {
                SLOG("[server] /stream resource releaser success=", success);
            });
    });

    svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });

    SLOG("[server] listening on 127.0.0.1:", port);
    svr.listen("127.0.0.1", port);
    return 0;
}
