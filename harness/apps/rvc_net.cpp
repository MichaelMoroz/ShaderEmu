#include "rvc_net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace {
double now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
// The machine a packet is for (its address is 10.0.H.L: H * 256 + L), 0xffff for everybody, 0 for nobody here.
unsigned target(const uint8_t* packet) {
    uint32_t to = (uint32_t)packet[16] << 24 | packet[17] << 16 | packet[18] << 8 | packet[19];
    if (to == 0xffffffffu || to == 0x0a00ffffu) return 0xffff;
    return (to >> 16) == 0x0a00 ? to & 0xffff : 0;
}
// RVC_NET_LOG=1: a line for every packet, on stderr.
void note(unsigned id, const char* what, const uint8_t* p, size_t n) {
    static const bool wanted = getenv("RVC_NET_LOG") != nullptr;
    if (!wanted || n < 20) return;
    unsigned head = (p[0] & 15) * 4, proto = p[9];
    bool ports = (proto == 6 || proto == 17) && n >= head + 4;
    fprintf(stderr, "[net %u] %7.3f %s %u.%u.%u.%u:%u > %u.%u.%u.%u:%u %s %zu bytes\n", id, now(), what, p[12], p[13], p[14], p[15],
            ports ? p[head] << 8 | p[head + 1] : 0, p[16], p[17], p[18], p[19], ports ? p[head + 2] << 8 | p[head + 3] : 0,
            proto == 6 ? "tcp" : proto == 17 ? "udp" : proto == 1 ? "icmp" : "other", n);
}
}

bool NetLink::open(int port, unsigned id, double loss, double delayMs, std::string& err) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "no winsock"; return false; }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons((u_short)(port + (int)id));
    if (s == INVALID_SOCKET || bind(s, (sockaddr*)&at, sizeof at) != 0) {
        err = "UDP port " + std::to_string(port + (int)id) + " is taken (another machine with number " + std::to_string(id) + "?)";
        if (s != INVALID_SOCKET) closesocket(s);
        return false;
    }
    u_long yes = 1;
    ioctlsocket(s, FIONBIO, &yes);
    // (a packet to a machine that is not running must not make the next receive fail)
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
    socket_ = (uintptr_t)s;
    port_ = port;
    id_ = id;
    loss_ = loss;
    delay_ = delayMs / 1000.0;
    rng_ = id * 2654435761u + (uint32_t)port + 1;
    return true;
}

void NetLink::pump() {
    if (!on()) return;
    SOCKET s = (SOCKET)socket_;
    double t = now();
    while (!out_.empty() && out_.front().due <= t) {
        const std::vector<uint8_t>& p = out_.front().bytes;
        unsigned to = target(p.data());
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (unsigned m = to == 0xffff ? 1 : to; m <= (to == 0xffff ? kBroadcastTo : to); ++m) {
            if (m == id_ || m == 0 || port_ + (int)m > 65535) continue;
            at.sin_port = htons((u_short)(port_ + (int)m));
            sendto(s, (const char*)p.data(), (int)p.size(), 0, (sockaddr*)&at, sizeof at);
        }
        out_.pop_front();
    }
    for (;;) {
        uint8_t buf[2048];
        int n = recv(s, (char*)buf, sizeof buf, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) break;
            continue;
        }
        unsigned to = n >= 20 && n <= (int)kMtu ? target(buf) : 0;
        if (to != 0xffff && to != id_) continue;
        // a guest that takes nothing loses the oldest
        if (in_.size() >= 64) { in_.pop_front(); ++dropped; }
        in_.emplace_back(buf, buf + n);
        note(id_, "in ", buf, (size_t)n);
        ++received;
    }
}

void NetLink::fromGuest(uint32_t sentSoFar, uint32_t taken, const uint32_t* window) {
    guestTail_ = taken;
    // (what a guest sent before this host first looked is not sent again)
    if (!tailKnown_) txTaken_ = sentSoFar;
    tailKnown_ = true;
    if (sentSoFar != txTaken_) {
        // The window's packets one after the other, each on 16 bytes, up to the one numbered
        // as the guest's word says: those not yet taken go out. (A window that does not hold
        // that one is of an older pass, or not a window: nothing is taken from it.)
        std::vector<std::pair<const uint8_t*, uint32_t>> fresh;
        bool whole = false;
        for (uint32_t at = 0; at + 16 <= kSlotBytes && !whole;) {
            const uint32_t* head = window + at / 4;
            uint32_t length = head[0], number = head[1];
            if (length < 20 || length > kMtu || at + 16 + length > kSlotBytes) break;
            if ((int32_t)(number - txTaken_) > 0) fresh.push_back({(const uint8_t*)(head + 4), length});
            whole = number == sentSoFar;
            at += 16 + ((length + 15) & ~15u);
        }
        if (whole)
            for (auto& p : fresh) {
                note(id_, "out", p.first, p.second);
                rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
                if ((rng_ % 10000) < (uint32_t)(loss_ * 100.0)) ++lost;
                else {
                    out_.push_back({now() + delay_, std::vector<uint8_t>(p.first, p.first + p.second)});
                    ++sent;
                }
            }
        // (a window that never comes whole is given up, or the guest would wait for ever)
        if (whole || ++stale_ > 200) {
            txTaken_ = sentSoFar;
            stale_ = 0;
        }
    }
    pump();
}

unsigned NetLink::toGuest(uint8_t* places, uint32_t& before) {
    pump();
    before = rxHead_;
    // only what the ring has room for, by what the guest was last seen to have taken
    uint32_t held = rxHead_ - guestTail_;
    if (!tailKnown_ || held > kSlots) return 0;
    unsigned n = 0;
    for (; n < kMost && held + n < kSlots && !in_.empty(); ++n) {
        uint8_t* place = places + (size_t)n * kSlotBytes;
        uint32_t head[4] = {(uint32_t)in_.front().size(), rxHead_ + n + 1, 0, 0};
        memset(place, 0, kSlotBytes);
        memcpy(place, head, 16);
        memcpy(place + 16, in_.front().data(), in_.front().size());
        in_.pop_front();
    }
    rxHead_ += n;
    return n;
}
