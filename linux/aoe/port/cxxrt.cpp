// The C++ runtime the game needs: allocation, and what a pure call or a guard asks for.
void* operator new(unsigned size) {
    void* p = malloc(size ? size : 1);
    if (!p) { fprintf(stderr, "aoe: out of memory, %u bytes\n", size); abort(); }
    return p;
}
void* operator new[](unsigned size) { return operator new(size); }
void operator delete(void* p) { free(p); }
void operator delete[](void* p) { free(p); }
void operator delete(void* p, unsigned) { free(p); }
void operator delete[](void* p, unsigned) { free(p); }
extern "C" void __cxa_pure_virtual(void) { fprintf(stderr, "aoe: pure call\n"); abort(); }
