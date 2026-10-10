// The host's side of the machine's network device (docs/lan.md), and the link between two
// hosts on this computer: a UDP socket each, at 127.0.0.1 and the port --net names plus the
// machine's number. rvc_harness and rvc_cpu share it; winsock stays in rvc_net.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class NetLink {
public:
    static const unsigned kMtu = 576, kSlotBytes = 640, kSlots = 8, kMost = 4;   // as the device's (src/gpu.h)
    static const unsigned kBroadcastTo = 16;   // a packet for everybody goes to machines 1 to this

    // loss in percent and delay in milliseconds are applied to every packet sent
    bool open(int port, unsigned id, double loss, double delayMs, std::string& err);
    bool on() const { return socket_ != ~(uintptr_t)0; }
    unsigned id() const { return id_; }

    // What a readback says: the guest's two words (packets sent, packets taken) and its window
    // of kSlotBytes (packets one after the other: length, number, two words unused, the bytes).
    // Those not yet taken go out.
    void fromGuest(uint32_t sent, uint32_t taken, const uint32_t* window);
    uint32_t txAck() const { return txTaken_; }
    // Packets for the guest in this pass: fills up to kMost places of kSlotBytes each and says
    // how many; `before` is the number delivered before them (_NetRxSeq).
    unsigned toGuest(uint8_t* places, uint32_t& before);

    uint64_t sent = 0, lost = 0, received = 0, dropped = 0;   // packets, for the run's summary

private:
    void pump();
    struct Pending { double due; std::vector<uint8_t> bytes; };
    uintptr_t socket_ = ~(uintptr_t)0;
    int port_ = 0;
    unsigned id_ = 0;
    double loss_ = 0, delay_ = 0;
    uint32_t rng_ = 1, txTaken_ = 0, rxHead_ = 0, guestTail_ = 0, stale_ = 0;
    bool tailKnown_ = false;
    std::deque<Pending> out_;
    std::deque<std::vector<uint8_t>> in_;
};
