// phase12 disconnect probe - E6: compile-only check of what Request exposes.
// The accessible public fields (remote_addr/remote_port/local_addr/local_port)
// are referenced here; attempts to reach a raw socket handle (fd / sock /
// socket_handle) are expected to fail at compile time. Compile with -c only.
#define _WIN32_WINNT 0x0600
#include <httplib.h>

int main() {
    httplib::Request req;

    // Public address/port fields, confirmed accessible.
    (void)req.remote_addr;
    (void)req.remote_port;
    (void)req.local_addr;
    (void)req.local_port;

    // Attempt to reach the raw socket handle behind the request.
    auto fd = req.fd;
    (void)fd;
    auto sock = req.sock;
    (void)sock;

    return 0;
}
