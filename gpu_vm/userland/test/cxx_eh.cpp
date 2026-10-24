// C++ on gpu_vm (gpu_vm/userland/README.md, "C++"): exceptions through frames of every kind the code generator makes
// (small, over 2 KB with sp moved in two steps, with fp and objects of variable size, realigned), destructors run
// while unwinding, rethrow, catch by base class and by ..., the library's own exceptions, run-time types.
//   gpuvm-linux-c++ -O2 cxx_eh.cpp -o cxx_eh      prints "cxx_eh: N of N passed"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

static int passed, total, destroyed;
#define CHECK(x) (total++, (x) ? (void)passed++ : (void)std::printf("cxx_eh: line %d: %s failed\n", __LINE__, #x))

struct Base { virtual ~Base() {} int code = 1; };
struct Derived : Base { Derived() { code = 7; } };
struct Guard { int *n; explicit Guard(int *p) : n(p) {} ~Guard() { ++*n; } };

__attribute__((noinline)) static void thrower(int kind) {
    Guard g(&destroyed);
    switch (kind) {
    case 0: throw 42;
    case 1: throw Derived();
    case 2: throw std::runtime_error("runtime");
    case 3: { std::vector<int> v(3); (void)v.at(10); break; }
    case 4: throw std::string("text");
    }
}

// a frame over 2 KB: sp moves in two steps around the callee-saved registers
__attribute__((noinline)) static int big(int kind, volatile int *sink) {
    volatile char buf[4096];
    Guard g(&destroyed);
    for (int i = 0; i < 4096; i += 512) buf[i] = (char)i;
    sink[0] = buf[512];
    thrower(kind);
    return buf[1024];
}

// fp and an object of variable size
__attribute__((noinline)) static int vla(int n, int kind) {
    volatile char *p = static_cast<volatile char *>(__builtin_alloca(n));
    Guard g(&destroyed);
    p[0] = 1;
    thrower(kind);
    return p[0];
}

// a realigned frame
__attribute__((noinline)) static int aligned(int kind) {
    alignas(64) volatile int a[16];
    Guard g(&destroyed);
    a[0] = kind;
    thrower(a[0]);
    return a[1];
}

// callee-saved registers live across the throw: their values must come back in the catching frame
__attribute__((noinline)) static int saved(int kind) {
    int s1 = kind * 3 + 1, s2 = kind * 5 + 2, s3 = kind * 7 + 3;
    try {
        thrower(kind);
    } catch (...) {
        return s1 + s2 + s3;
    }
    return -1;
}

__attribute__((noinline)) static void rethrower() {
    try {
        thrower(1);
    } catch (Base &) {
        throw;
    }
}

int main() {
    volatile int sink[4];
    destroyed = 0;
    try { thrower(0); } catch (int e) { CHECK(e == 42); }
    CHECK(destroyed == 1);
    try { thrower(1); } catch (Base &b) { CHECK(b.code == 7); CHECK(typeid(b) == typeid(Derived)); }
    try { thrower(2); } catch (std::exception &e) { CHECK(std::strcmp(e.what(), "runtime") == 0); }
    try { thrower(3); } catch (std::out_of_range &) { CHECK(true); }
    try { thrower(4); } catch (const std::string &s) { CHECK(s == "text"); }
    destroyed = 0;
    try { big(0, sink); } catch (int e) { CHECK(e == 42); }
    CHECK(destroyed == 2);
    destroyed = 0;
    try { vla(100, 1); } catch (Derived &d) { CHECK(d.code == 7); }
    CHECK(destroyed == 2);
    destroyed = 0;
    try { aligned(2); } catch (std::runtime_error &) { CHECK(true); }
    CHECK(destroyed == 2);
    CHECK(saved(0) == 1 + 2 + 3);
    CHECK(saved(4) == 13 + 22 + 31);
    try { rethrower(); } catch (Derived &) { CHECK(true); }
    try { try { thrower(0); } catch (...) { throw std::logic_error("again"); } } catch (std::logic_error &e) { CHECK(std::string(e.what()) == "again"); }
    // the library at work
    std::map<std::string, std::unique_ptr<Base>> m;
    m["a"] = std::make_unique<Derived>();
    CHECK(m.at("a")->code == 7);
    try { (void)m.at("b"); } catch (std::out_of_range &) { CHECK(true); }
    std::printf("cxx_eh: %d of %d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
