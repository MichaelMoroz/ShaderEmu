// Timed events for the key ring from the command line, for both harnesses (docs/machine/input.md,
// "More of a mouse and the hands"): what a test gives the guest that a window or the world would.
//
//   --scroll S,W,N        at second S, N notches (signed: up or right positive) of wheel W:
//                         y, x  the pointer's own wheel (a mouse's, tilted for x)
//                         ly, lx  the left hand's stick;  ry, rx  the right hand's
//   --input-event S,HEX   at second S, the ring event HEX as it is (bit 31 a press)
//
// The buttons word's other bits (back, forward, the hands' buttons) are --pointer-button's B,
// which takes hex too (0x400: the right hand's trigger).
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct TimedInput {
    double at;
    unsigned event;
};

// The ring's codes that are no keys (linux/kernel/shaderemu_input.c has the same).
enum : unsigned { kWheelLeft = 0x3fc, kWheelRight = 0x3fd, kWheelUp = 0x3fe, kWheelDown = 0x3ff };
enum : unsigned { kFromPointer = 0, kFromLeftHand = 1, kFromRightHand = 2 };   // bits 12-13 of a wheel's event

inline unsigned wheel_event(unsigned code, unsigned from) { return 0x80000000u | (from & 3) << 12 | code; }

// Parse one switch's value into `into`; false (with a line on stderr) for one that makes no sense.
inline bool parse_scroll(const std::string& v, std::vector<TimedInput>& into) {
    double at = 0;
    char wheel[8] = {};
    int n = 0;
    if (sscanf(v.c_str(), "%lf,%3[a-z],%d", &at, wheel, &n) != 3) {
        fprintf(stderr, "--scroll S,W,N: W is y, x, ly, lx, ry or rx (%s)\n", v.c_str());
        return false;
    }
    std::string w = wheel;
    unsigned from = w[0] == 'l' ? kFromLeftHand : w[0] == 'r' ? kFromRightHand : kFromPointer;
    bool across = w.back() == 'x';
    if (w != "x" && w != "y" && w != "lx" && w != "ly" && w != "rx" && w != "ry") {
        fprintf(stderr, "--scroll S,W,N: W is y, x, ly, lx, ry or rx (%s)\n", v.c_str());
        return false;
    }
    unsigned code = across ? (n > 0 ? kWheelRight : kWheelLeft) : (n > 0 ? kWheelUp : kWheelDown);
    for (int i = 0; i < std::abs(n); i++) into.push_back({at, wheel_event(code, from)});
    std::stable_sort(into.begin(), into.end(), [](const TimedInput& a, const TimedInput& b) { return a.at < b.at; });
    return true;
}

inline bool parse_input_event(const std::string& v, std::vector<TimedInput>& into) {
    double at = 0;
    unsigned e = 0;
    if (sscanf(v.c_str(), "%lf,%x", &at, &e) != 2) {
        fprintf(stderr, "--input-event S,HEX (%s)\n", v.c_str());
        return false;
    }
    into.push_back({at, e});
    std::stable_sort(into.begin(), into.end(), [](const TimedInput& a, const TimedInput& b) { return a.at < b.at; });
    return true;
}

// --pointer-button B,S,T, given as often as wanted: the buttons word is the bits of every one whose
// seconds hold now (a hand's trigger held while another button comes and goes)
struct ButtonSpan {
    unsigned bits;
    double from, to;
};

inline bool parse_button(const std::string& v, std::vector<ButtonSpan>& into) {
    ButtonSpan b = {};
    int bits = 0;
    if (sscanf(v.c_str(), "%i,%lf,%lf", &bits, &b.from, &b.to) != 3) {
        fprintf(stderr, "--pointer-button B,S,T (%s)\n", v.c_str());
        return false;
    }
    b.bits = (unsigned)bits;
    into.push_back(b);
    return true;
}

inline unsigned buttons_at(const std::vector<ButtonSpan>& spans, double now) {
    unsigned bits = 0;
    for (const ButtonSpan& b : spans)
        if (now >= b.from && now < b.to) bits |= b.bits;
    return bits;
}

// The events due by second `now`, at most `room` of them, from `next` on.
inline int due_inputs(const std::vector<TimedInput>& list, size_t& next, double now, unsigned* out, int room) {
    int n = 0;
    while (n < room && next < list.size() && list[next].at <= now) out[n++] = list[next++].event;
    return n;
}
