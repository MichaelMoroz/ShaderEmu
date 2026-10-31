/* qemu has none of the machine's CSRs: the libc's fill and copy, plainly. */
typedef unsigned size_t_;
void* memset(void* d, int c, size_t_ n) { char* p = d; while (n--) *p++ = c; return d; }
void* memcpy(void* d, const void* s, size_t_ n) { char* p = d; const char* q = s; while (n--) *p++ = *q++; return d; }
void* memmove(void* d, const void* s, size_t_ n) {
    char* p = d; const char* q = s;
    if (p < q) while (n--) *p++ = *q++;
    else while (n--) p[n] = q[n];
    return d;
}
