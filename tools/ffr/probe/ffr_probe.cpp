// FINAL FANTASY RESONANCE development probe (not shipped): serves guest memory reads to a computer over
// a loopback TCP socket, for reverse engineering through `adb forward tcp:47123 tcp:47123`.
// Protocol (little-endian): request u8 op, u64 addr, u32 len.
//   op 1 read: reply u8 ok, then len bytes when ok (len <= 16 MiB).
//   op 2 info: reply u64 main_base, u64 main_size, u64 heap_begin, u64 heap_end, u64 tick.
//   op 3 mapped: reply u8 is_mapped(addr, len).
// Reads go through the host's read_memory, which checks the range is mapped.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mods/dsmod_module_abi.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kTitle = 0x0100606026F12000ull;
constexpr int kPort = 47123;

struct Probe {
    EdenDsmodHostApi host{};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> tick{0};
    int listen_fd = -1;
    std::thread th;
};

bool SendAll(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        c += w; n -= size_t(w);
    }
    return true;
}
bool RecvAll(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n) {
        ssize_t r = recv(fd, c, n, 0);
        if (r <= 0) return false;
        c += r; n -= size_t(r);
    }
    return true;
}

void Serve(Probe* p, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    std::vector<uint8_t> buf;
    for (;;) {
        uint8_t hdr[13];
        if (!RecvAll(fd, hdr, sizeof hdr)) break;
        uint8_t op = hdr[0];
        uint64_t addr; uint32_t len;
        std::memcpy(&addr, hdr + 1, 8); std::memcpy(&len, hdr + 9, 4);
        if (op == 1) {
            if (len > (16u << 20)) len = 0;
            buf.resize(len);
            uint8_t ok = len && p->host.read_memory(p->host.userdata, addr, buf.data(), len) ? 1 : 0;
            if (!SendAll(fd, &ok, 1)) break;
            if (ok && !SendAll(fd, buf.data(), len)) break;
        } else if (op == 2) {
            uint64_t v[5] = {p->host.main_base, p->host.main_size,
                             p->host.get_heap_begin ? p->host.get_heap_begin(p->host.userdata) : 0,
                             p->host.get_heap_end ? p->host.get_heap_end(p->host.userdata) : 0,
                             p->tick.load()};
            if (!SendAll(fd, v, sizeof v)) break;
        } else if (op == 3) {
            uint8_t ok = p->host.is_mapped && p->host.is_mapped(p->host.userdata, addr, len) ? 1 : 0;
            if (!SendAll(fd, &ok, 1)) break;
        } else {
            break;
        }
    }
    close(fd);
}

void Listen(Probe* p) {
    while (!p->stop.load()) {
        int fd = accept(p->listen_fd, nullptr, nullptr);
        if (fd < 0) { if (p->stop.load()) break; usleep(100000); continue; }
        Serve(p, fd);  // one client at a time
    }
}

EdenDsmodBool SupportsBuild(const char* build) {
    return build && std::strncmp(build, "84E182F72EE1DA4F", 16) == 0 ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    auto* p = new Probe();
    p->host = *host;
    p->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(kPort); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char msg[160];
    if (bind(p->listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(p->listen_fd, 1) == 0) {
        p->th = std::thread(Listen, p);
        std::snprintf(msg, sizeof msg, "ffr_probe: listening on 127.0.0.1:%d, main %#llx size %#llx", kPort,
                      (unsigned long long)host->main_base, (unsigned long long)host->main_size);
    } else {
        std::snprintf(msg, sizeof msg, "ffr_probe: bind failed (%s)", std::strerror(errno));
    }
    host->log(host->userdata, EDEN_DSMOD_LOG_INFO, msg);
    return p;
}

void Destroy(void* inst) {
    auto* p = static_cast<Probe*>(inst);
    p->stop = true;
    if (p->listen_fd >= 0) { shutdown(p->listen_fd, SHUT_RDWR); close(p->listen_fd); }
    if (p->th.joinable()) p->th.join();
    delete p;
}

void Sample(void* inst, const EdenDsmodHostApi* host) {
    auto* p = static_cast<Probe*>(inst);
    uint64_t t = p->tick.fetch_add(1) + 1;
    host->publish_i64(host->userdata, "probe.tick", int64_t(t));
}
void Tick(void*, const EdenDsmodHostApi*) {}

const EdenDsmodModuleApi kApi = {
    EDEN_DSMOD_MODULE_ABI_VERSION, sizeof(EdenDsmodModuleApi), 0, EDEN_DSMOD_MODULE_ABI_HASH,
    kTitle, "FF Resonance probe", 0, SupportsBuild, Create, Destroy, Sample, Tick,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const EdenDsmodModuleApi*
eden_dsmod_get_module(uint32_t abi, uint64_t hash) {
    return abi == EDEN_DSMOD_MODULE_ABI_VERSION && hash == EDEN_DSMOD_MODULE_ABI_HASH ? &kApi : nullptr;
}
