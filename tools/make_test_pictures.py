"""Makes the image's two test pictures and checks the guest's decoder against Pillow's.

    python tools/make_test_pictures.py            the pictures, into linux/apps/pictures
    python tools/make_test_pictures.py --check    builds linux/apps/ui_image.h for this computer (gcc in
                                                  WSL), decodes both and compares with Pillow; prints
                                                  the sums `NXVIEW_SUM=1 nxview --decode` must print

The pictures are cut from the desktop's own picture (linux/prebuilt's wallpaper): a PNG of
8-bit colours and a baseline JPEG with its colours at half size, the common kinds.
"""
import os, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, 'linux', 'apps', 'pictures')
SOURCE = os.path.join(HERE, 'linux', 'prebuilt', 'root', 'usr', 'share', 'wallpaper-fox.ppm')

STUB = r'''
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t (*mcw_fn)(uint32_t a0, uint32_t a1);
static int mcw_open(int n) { (void)n; return 0; }
static void mcw_close(void) {}
static void *mcw_alloc(unsigned n) { return calloc(1, (n + 15) & ~15u); }
static void mcw_touch(void *m, unsigned n) { (void)m; (void)n; }
static void mcw_post(int k, mcw_fn f, uint32_t a, uint32_t b) { (void)k; (void)f; (void)a; (void)b; }
static int mcw_wait(int k) { (void)k; return 0; }
'''

MAIN = r'''
#include "ui_image.h"
int main(int argc, char **argv) {
    int w, h;
    const char *why = ui_image_to_ppm(argv[1], argv[2], &w, &h, 0);
    if (why) { fprintf(stderr, "%s: %s\n", argv[1], why); return 1; }
    FILE *f = fopen(argv[2], "rb");
    unsigned sum = 0, n = 0; int c;
    while ((c = fgetc(f)) != EOF) sum = (sum << 5 | sum >> 27) ^ (unsigned)c, n++;
    printf("%s: %d x %d, sum %08x of %u bytes\n", argv[1], w, h, sum, n);
    return 0;
}
'''


def make():
    os.makedirs(OUT, exist_ok=True)
    picture = Image.open(SOURCE).convert('RGB')
    w, h = picture.size
    side = min(w, h)
    picture = picture.crop(((w - side * 4 // 3) // 2 if side * 4 // 3 <= w else 0, 0, (w + side * 4 // 3) // 2 if side * 4 // 3 <= w else w, side))
    picture = picture.resize((640, 480), Image.LANCZOS)
    picture.save(os.path.join(OUT, 'picture-fox.png'), optimize=True)
    picture.save(os.path.join(OUT, 'picture-fox.jpg'), quality=85, subsampling='4:2:0', progressive=False)
    for name in ('picture-fox.png', 'picture-fox.jpg'):
        print(name, os.path.getsize(os.path.join(OUT, name)), 'bytes')


def wsl(path):
    path = os.path.abspath(path).replace('\\', '/')
    return '/mnt/' + path[0].lower() + path[2:]


def check():
    build = os.path.join(HERE, 'build', 'ui_image_test')
    os.makedirs(build, exist_ok=True)
    open(os.path.join(build, 'mcw.h'), 'w', newline='\n').write(STUB)
    open(os.path.join(build, 'main.c'), 'w', newline='\n').write(MAIN)
    subprocess.check_call(['wsl', '-e', 'gcc', '-O2', '-w', '-I' + wsl(build), '-I' + wsl(os.path.join(HERE, 'linux', 'apps')),
                           wsl(os.path.join(build, 'main.c')), '-o', wsl(os.path.join(build, 'decode'))])
    for name in ('picture-fox.png', 'picture-fox.jpg'):
        out = os.path.join(build, name + '.ppm')
        print(subprocess.check_output(['wsl', '-e', wsl(os.path.join(build, 'decode')), wsl(os.path.join(OUT, name)), wsl(out)]).decode().strip())
        ours = Image.open(out).convert('RGB')
        theirs = Image.open(os.path.join(OUT, name)).convert('RGB')
        assert ours.size == theirs.size, (ours.size, theirs.size)
        a, b = ours.tobytes(), theirs.tobytes()
        worst = max(abs(x - y) for x, y in zip(a, b))
        off = sum(1 for x, y in zip(a, b) if abs(x - y) > 2)
        print('  against Pillow: the largest difference is %d levels; %d of %d bytes differ by more than 2' % (worst, off, len(a)))


if __name__ == '__main__':
    if '--check' in sys.argv:
        check()
    else:
        make()
