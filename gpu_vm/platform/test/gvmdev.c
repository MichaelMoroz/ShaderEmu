// gvmdev: the machine's devices from a gpu_vm Linux program (gpu_vm/platform.md, "Tests"), each through its driver:
//   fb      /dev/fb0: the display in 32-bit pixels, a picture drawn and read back, the control words' mode and size
//   gpu     /dev/gpu: a list (a clear and a rectangle) drawn by the card and its picture copied into RAM
//           (SUBMIT_INTO), checked pixel by pixel; the lists and copies counted
//   input   /dev/input/event1 (the pointer) for SECONDS: its moves and buttons (rvc_harness --pointer-sweep 0
//           --pointer-button 1,S,T moves it and presses the left button)
//   sound   /dev/sound: half a second of a tone written as PCM; the card's clock must move
//   gvmdev [fb|gpu|input|sound|all] [SECONDS]       "gvmdev: PASS" when every check holds ("no card" is no fault)
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CTRL_OFFSET 0x01000000u   // /dev/gpu's offset of the control words (RISC-V 0x87000000; gpu_vm 0x01000000)
#define LIST_OFFSET 0x01500000u   // a command list (programs/common/gpu.h's GPU_COMMANDS)
#define INTO_OFFSET 0x01600000u   // where the picture is copied
#define V(p, off) (*(volatile uint32_t *)((char *)(p) + (off)))

struct gpu_submit { uint32_t list, count, how, address, width, height, row; };
#define GPU_SUBMIT _IOW('G', 2, struct gpu_submit)

static int failed;
static void check(int ok, const char *what) {
    printf("gvmdev: %s: %s\n", what, ok ? "ok" : "WRONG");
    failed += !ok;
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void test_fb(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { printf("gvmdev: fb: no /dev/fb0 (%s)\n", strerror(errno)); failed++; return; }
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { check(0, "fb: its mode"); close(fd); return; }
    printf("gvmdev: fb: %u x %u, %u bits a pixel, %u bytes a row\n", v.xres, v.yres, v.bits_per_pixel, f.line_length);
    uint32_t *px = mmap(0, f.line_length * v.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(px != MAP_FAILED, "fb: mapped");
    if (px == MAP_FAILED) { close(fd); return; }
    for (uint32_t y = 0; y < v.yres; y++)
        for (uint32_t x = 0; x < v.xres; x++)
            px[y * (f.line_length / 4) + x] = (x * 255 / v.xres) << 16 | (y * 255 / v.yres) << 8 | ((x ^ y) & 0x40 ? 0xc0 : 0x30);
    uint32_t sum = 0, bad = 0;
    for (uint32_t y = 0; y < v.yres; y++)
        for (uint32_t x = 0; x < v.xres; x++) {
            uint32_t want = (x * 255 / v.xres) << 16 | (y * 255 / v.yres) << 8 | ((x ^ y) & 0x40 ? 0xc0 : 0x30);
            bad += px[y * (f.line_length / 4) + x] != want;
            sum = sum * 31 + px[y * (f.line_length / 4) + x];
        }
    printf("gvmdev: fb: the picture's sum %08x\n", sum);
    check(bad == 0, "fb: every pixel as drawn");
    int g = open("/dev/gpu", O_RDWR);
    if (g >= 0) {
        void *ctl = mmap(0, 4096, PROT_READ, MAP_SHARED, g, CTRL_OFFSET);
        if (ctl != MAP_FAILED) {
            printf("gvmdev: fb: the display's words: mode %u, %u x %u\n", V(ctl, 0), V(ctl, 4), V(ctl, 8));
            check(V(ctl, 0) == 1 && V(ctl, 4) == v.xres && V(ctl, 8) == v.yres, "fb: the display shows it");
            munmap(ctl, 4096);
        }
        close(g);
    }
    munmap(px, f.line_length * v.yres);
    close(fd);
}

static void test_gpu(void) {
    int g = open("/dev/gpu", O_RDWR);
    if (g < 0) { printf("gvmdev: gpu: no /dev/gpu (%s)\n", strerror(errno)); failed++; return; }
    char *mem = mmap(0, 0x00800000, PROT_READ | PROT_WRITE, MAP_SHARED, g, CTRL_OFFSET);
    if (mem == MAP_FAILED) { check(0, "gpu: mapped"); close(g); return; }
    uint32_t *list = (uint32_t *)(mem + (LIST_OFFSET - CTRL_OFFSET)), *into = (uint32_t *)(mem + (INTO_OFFSET - CTRL_OFFSET));
    const uint32_t W = 64, H = 48;
    for (uint32_t i = 0; i < W * H; i++) into[i] = 0xdeadbeef;
    memset(list, 0, 3 * 64);
    list[0] = 1; list[1] = 0x00123456; list[3] = 0;                      // clear (3 vertices from 0)
    list[16] = 2; list[17] = 0x00ff8000; list[19] = 3;                   // a rectangle (6 vertices from 3)
    list[20] = 16; list[21] = 8; list[22] = 48; list[23] = 40; list[24] = 0;
    uint32_t frames = V(mem, 0x1c), copies = V(mem, 0x38);
    // (the device window's addresses: gpu_vm's are the RISC-V machine's less 0x86000000; the mapping is at its own)
    struct gpu_submit s = {(uint32_t)(uintptr_t)list, 2, 1 | 4, (uint32_t)(uintptr_t)into, W, H, W};
    printf("gvmdev: gpu: the list at %08x, its picture into %08x\n", s.list, s.address);
    int r = ioctl(g, GPU_SUBMIT, &s);
    check(r == 0, "gpu: submitted and drawn");
    double t0 = now();
    while (V(mem, 0x38) == copies && now() - t0 < 5) usleep(1000);
    printf("gvmdev: gpu: lists drawn %u -> %u, copies %u -> %u, %.0f ms for the copy\n", frames, V(mem, 0x1c), copies, V(mem, 0x38), (now() - t0) * 1000);
    check(V(mem, 0x1c) != frames && V(mem, 0x38) != copies, "gpu: the list drawn and its picture copied");
    uint32_t bad = 0, inside = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t in = x >= 16 && x < 48 && y >= 8 && y < 40, want = in ? 0x00ff8000 : 0x00123456, got = into[y * W + x] & 0xffffff;
            if (got != want && bad++ < 4) printf("gvmdev: gpu: pixel %u,%u is %08x, not %08x\n", x, y, into[y * W + x], want);
            inside += in;
        }
    printf("gvmdev: gpu: %u of %u pixels as drawn (%u of the rectangle's)\n", W * H - bad, W * H, inside);
    check(bad == 0, "gpu: the picture in RAM");
    munmap(mem, 0x00800000);
    close(g);
}

static void test_input(int seconds) {
    int fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (fd < 0) { printf("gvmdev: input: no /dev/input/event1 (%s)\n", strerror(errno)); failed++; return; }
    char name[64] = "";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    int moves = 0, presses = 0, releases = 0, lastx = -1, lasty = -1;
    double t0 = now();
    unsigned char keys[KEY_MAX / 8 + 1];
    memset(keys, 0, sizeof keys);
    if (ioctl(fd, EVIOCGKEY(sizeof keys), keys) >= 0 && (keys[BTN_LEFT / 8] >> (BTN_LEFT % 8) & 1)) presses++;   // (down already)
    while (now() - t0 < seconds) {
        struct input_event e;
        ssize_t n = read(fd, &e, sizeof e);
        if (n != sizeof e) { usleep(5000); continue; }
        if (e.type == EV_ABS && e.code == ABS_X) lastx = e.value, moves++;
        if (e.type == EV_ABS && e.code == ABS_Y) lasty = e.value;
        if (e.type == EV_KEY && e.code == BTN_LEFT) e.value ? presses++ : releases++;
    }
    printf("gvmdev: input: %s: %d moves (last at %d, %d), the left button down %d and up %d times\n", name, moves, lastx, lasty, presses, releases);
    check(moves > 0, "input: the pointer moves");
    check(presses > 0, "input: the left button");
    close(fd);
}

static void test_sound(void) {
    int fd = open("/dev/sound", O_RDWR);
    if (fd < 0) { printf("gvmdev: sound: no card (%s): nothing to test\n", strerror(errno)); return; }
    volatile uint32_t *regs = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    check(regs != MAP_FAILED, "sound: its registers mapped");
    if (regs == MAP_FAILED) { close(fd); return; }
    uint32_t clock0 = regs[0x230 / 4], rate = regs[0x234 / 4];
    usleep(200000);
    printf("gvmdev: sound: enabled %u, voices %u; the card's clock %u -> %u in 0.2 s, at %u samples a second\n", regs[0x220 / 4],
           regs[0x228 / 4], clock0, regs[0x230 / 4], rate);
    int16_t tone[11025];   // half a second at 22,050 Hz
    for (int i = 0; i < 11025; i++) tone[i] = (int16_t)(8000 * sin(2 * 3.14159265 * 440 * i / 22050.0));
    ssize_t n = write(fd, tone, sizeof tone);
    usleep(300000);
    uint32_t clock1 = regs[0x230 / 4];
    printf("gvmdev: sound: %zd bytes written; the card's clock %u -> %u at %u samples a second\n", n, clock0, clock1, rate);
    check(n == (ssize_t)sizeof tone, "sound: the tone written");
    check(clock1 != clock0 && rate != 0, "sound: the card mixes");
    close(fd);
}

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "all";
    int seconds = argc > 2 ? atoi(argv[2]) : 3;
    int all = !strcmp(what, "all");
    if (all || !strcmp(what, "fb")) test_fb();
    if (all || !strcmp(what, "gpu")) test_gpu();
    if (all || !strcmp(what, "sound")) test_sound();
    if (all || !strcmp(what, "input")) {
        int fd = open("/dev/fb0", O_RDWR);   // (the pointer is in display pixels: the display on while it is read)
        test_input(seconds);
        if (fd >= 0) close(fd);
    }
    printf("gvmdev: %s\n", failed ? "FAIL" : "PASS");
    return failed != 0;
}
