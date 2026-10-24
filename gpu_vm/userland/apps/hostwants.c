/* What the host asks of the guest at boot, as linux/userland/emuinit.c reads it: the control words' texel 0x3c
 * through /dev/gpu. Exits with its bits: 1 the window system and its desktop, 2 the console as tabs (emumux). */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    int fd = open("/dev/gpu", O_RDWR), wanted = 0;
    const unsigned* words;

    if (fd < 0) return 0;
    words = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);
    if (words != MAP_FAILED) wanted = words[0x3c / 4] & 3;
    close(fd);
    return wanted;
}
