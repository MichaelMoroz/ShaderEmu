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
        return address.rfind("persist:", 0) == 0 || address == "host:name" || address.rfind("clip:", 0) == 0 ||
               address.rfind("photo:", 0) == 0;
    }

    // "photo:show=TEXEL,ENTRIES,TAIL,NAME" (docs/apps/export.md, "A photo"): the world draws a zip into VRChat's camera's
    // pictures (Photo.shader). Here RVC_PHOTO=FILE gets the 24-bit bitmap such a picture saved from Paint is, at
    // RVC_PHOTO_SIZE=WxH (1920x1080), by the shader's own sums; `ram` is the machine's RAM from 0x80000000, `ramSize` long.
    // Without RVC_PHOTO a photo is shown (200) and nothing is written, as a world with no camera in the cube.
    static inline const uint8_t* ram = nullptr;
    static inline size_t ramSize = 0;

    // The byte of the bitmap file at `f` (f >= 54): the entries after the header, the directory at the end, its start's word made here.
    static uint8_t photoByte(uint64_t f, uint64_t fileSize, const uint8_t* at, uint32_t entries, uint32_t tail) {
        uint64_t tailAt = fileSize - tail;
        if (f - 54 < entries) return at[f - 54];
        if (f < tailAt) return 0;
        uint64_t t = f - tailAt;
        if (t >= tail - 6 && t < tail - 2) return (uint8_t)(tailAt >> (8 * (t - (tail - 6))));   // (the end's word 16: where the directory begins)
        return at[entries + t];
    }

    static unsigned photoAnswer(const std::string& what) {
        if (what == "hide") return 200;
        if (what.rfind("show=", 0) != 0) return 400;
        unsigned texel = 0, entries = 0, tail = 0, w = 1920, h = 1080;
        if (sscanf(what.c_str() + 5, "%u,%u,%u", &texel, &entries, &tail) != 3 || tail < 22) return 400;
        const char* file = getenv("RVC_PHOTO");
        if (!file || !*file) return 200;
        const char* size = getenv("RVC_PHOTO_SIZE");
        if (size && *size) sscanf(size, "%ux%u", &w, &h);
        uint64_t row = (uint64_t)w * 3, fileSize = 54 + row * h;
        if (row % 4 || 54 + (uint64_t)entries + tail > fileSize || !ram || (uint64_t)texel * 16 + entries + tail > ramSize) return 413;
        const uint8_t* at = ram + (size_t)texel * 16;
        std::vector<uint8_t> bmp((size_t)fileSize);
        uint8_t* b = bmp.data();
        auto le = [&](size_t o, uint32_t v, int n) { for (int i = 0; i < n; i++) b[o + i] = (uint8_t)(v >> (8 * i)); };
        b[0] = 'B', b[1] = 'M', le(2, (uint32_t)fileSize, 4), le(10, 54, 4), le(14, 40, 4), le(18, w, 4), le(22, h, 4);
        le(26, 1, 2), le(28, 24, 2), le(34, (uint32_t)(row * h), 4), le(38, 3780, 4), le(42, 3780, 4);   // (Paint's: 96 dots an inch)
        // the picture's pixels, as the shader draws them: (x, row from the bottom) holds the file's bytes from
        // 54 + row * w * 3 + x * 3, blue, green, red; written here by their place in the file, which is the same
        for (uint64_t f = 54; f < fileSize; f++) b[f] = photoByte(f, fileSize, at, entries, tail);
        FILE* out = fopen(file, "wb");
        if (!out) return 500;
        bool fine = fwrite(b, 1, bmp.size(), out) == bmp.size();
        return fclose(out) == 0 && fine ? 200 : 500;
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
        if (address.rfind("photo:", 0) == 0) return photoAnswer(address.substr(6));
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
