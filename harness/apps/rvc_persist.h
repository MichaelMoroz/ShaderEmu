// The host's part of the guest's kept folder (docs/system/persist.md): the addresses "persist:..."
// of a fetch are answered from a file, which is to a harness what PlayerData is to the world.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct PersistHost {
    static const unsigned kLimit = 100000;   // bytes a player's data may be
    std::string file;                        // --persist FILE; FILE.auto says "0" when loading at start is off

    static bool is(const std::string& address) {
        return address.rfind("persist:", 0) == 0 || address == "host:name" || address.rfind("clip:", 0) == 0;
    }
    static bool clip(const std::string& address) { return address.rfind("clip:", 0) == 0; }
    static const unsigned kClipMost = 0x40000;   // what the guest hands over for "clip:show" and "clip:set": the fetch's 256 KB

    // "clip:..." (docs/system/clipboard.md): the field a world shows in front of the screen is files here, RVC_CLIP=FILE.
    // show: FILE gets the text (after the guest's line of 256 bytes, which FILE.title gets); set: FILE.board the guest's
    // clipboard; ask: the field asked for text; take: FILE.in, what a visitor would have pasted, given once and then gone
    // (404 until there is one); hide. Without RVC_CLIP every one is 404, as a host with no field.
    static unsigned clipAnswer(const std::string& what, unsigned offset, const uint8_t* data, std::vector<uint8_t>& reply) {
        const char* base = getenv("RVC_CLIP");
        if (!base || !*base) return 404;
        std::string file(base);
        auto write = [](const std::string& name, const uint8_t* p, size_t n) {
            FILE* f = fopen(name.c_str(), "wb");
            if (!f) return false;
            bool fine = fwrite(p, 1, n, f) == n;
            return fclose(f) == 0 && fine;
        };
        if (offset > kClipMost) return 413;
        if (what == "show") {
            if (offset < 256) return 400;
            size_t title = strnlen((const char*)data, 256);
            return write(file + ".title", data, title) && write(file, data + 256, offset - 256) ? 200 : 500;
        }
        if (what == "set") return write(file + ".board", data, offset) ? 200 : 500;
        if (what == "ask" || what == "hide") return 200;
        if (what == "take") {
            // (RVC_CLIP_IN=FILE: that file is the paste, given once a run and kept: a test's, tools/test_all.py)
            static bool givenIn;
            const char* in = getenv("RVC_CLIP_IN");
            bool once = in && *in && !givenIn;
            FILE* f = fopen(once ? in : (file + ".in").c_str(), "rb");
            if (once && f) givenIn = true;
            if (!f) return 404;
            uint8_t block[4096];
            size_t n;
            while ((n = fread(block, 1, sizeof block, f)) > 0 && reply.size() < kClipMost) reply.insert(reply.end(), block, block + n);
            fclose(f);
            if (reply.size() > kClipMost) reply.resize(kClipMost);
            if (!once) remove((file + ".in").c_str());
            return 200;
        }
        return 400;
    }

    // The player's name for "host:name" (docs/system/persist.md, "The player's name"): SHADEREMU_PLAYER,
    // else this computer's user, else "player".
    static std::string player() {
        const char* name = getenv("SHADEREMU_PLAYER");
        if (!name || !*name) name = getenv("USERNAME");
        return name && *name ? std::string(name) : std::string("player");
    }

    bool stored(std::vector<uint8_t>& to) const {
        FILE* f = file.empty() ? nullptr : fopen(file.c_str(), "rb");
        if (!f) return false;
        uint8_t block[4096];
        size_t n;
        to.clear();
        while ((n = fread(block, 1, sizeof block, f)) > 0) to.insert(to.end(), block, block + n);
        fclose(f);
        return !to.empty();
    }

    bool automatic() const {
        FILE* f = fopen((file + ".auto").c_str(), "rb");
        int c = f ? fgetc(f) : '1';
        if (f) fclose(f);
        return c != '0';
    }

    // The status for an address; `offset` is the request's fourth word, `data` what the guest
    // left in the fetch's 256 KB, and `reply` gets the bytes to put there (none: left as it is).
    unsigned answer(const std::string& address, unsigned offset, const uint8_t* data, std::vector<uint8_t>& reply) const {
        reply.clear();
        if (clip(address)) return clipAnswer(address.substr(5), offset, data, reply);
        std::string what = address.substr(8);
        if (address == "host:name") {
            std::string name = player();
            reply.assign(name.begin(), name.end());
            return 200;
        }
        if (file.empty()) return 404;
        if (what == "load" || what == "load!") {
            if (what == "load" && !automatic()) return 403;
            return stored(reply) ? 200 : 404;
        }
        if (what == "save") {
            if (offset > kLimit) return 413;
            FILE* f = fopen(file.c_str(), "wb");
            if (!f) return 500;
            bool fine = fwrite(data, 1, offset, f) == offset;
            return fclose(f) == 0 && fine ? 200 : 500;
        }
        if (what == "clear") {
            remove(file.c_str());
            return 200;
        }
        if (what == "auto=0" || what == "auto=1") {
            FILE* f = fopen((file + ".auto").c_str(), "wb");
            if (!f) return 500;
            fputc(what[5], f);
            fclose(f);
            return 200;
        }
        if (what == "state") {
            std::vector<uint8_t> blob;
            char text[96];
            stored(blob);
            int n = snprintf(text, sizeof text, "stored %u\nlimit %u\nauto %d\n", (unsigned)blob.size(), kLimit, automatic() ? 1 : 0);
            reply.assign(text, text + n);
            return 200;
        }
        return 400;
    }
};
