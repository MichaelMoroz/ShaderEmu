// The C++ runtime Tiberian Dawn needs, in place of libstdc++ (the toolchain's is built for
// glibc and an FPU): allocation, the string class's code, the clock, and what a failure calls.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <time.h>

void* operator new(std::size_t size)
{
    void* p = malloc(size ? size : 1);
    if (!p) {
        fputs("tdawn: out of memory\n", stderr);
        abort();
    }
    return p;
}
void* operator new[](std::size_t size)
{
    return operator new(size);
}
void operator delete(void* p) noexcept
{
    free(p);
}
void operator delete[](void* p) noexcept
{
    free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    free(p);
}

// the headers only declare these: std::string's functions that were not inlined
template class std::basic_string<char>;

static void fail(const char* what, const char* text)
{
    fprintf(stderr, "tdawn: %s: %s\n", what, text ? text : "");
    abort();
}

namespace std
{
    void __throw_length_error(const char* s)
    {
        fail("length error", s);
    }
    void __throw_logic_error(const char* s)
    {
        fail("logic error", s);
    }
    void __throw_out_of_range(const char* s)
    {
        fail("out of range", s);
    }
    void __throw_out_of_range_fmt(const char* s, ...)
    {
        fail("out of range", s);
    }
    void __throw_invalid_argument(const char* s)
    {
        fail("invalid argument", s);
    }
    void __throw_bad_alloc()
    {
        fail("bad alloc", 0);
    }
    void __throw_bad_array_new_length()
    {
        fail("bad array length", 0);
    }
    void __throw_bad_function_call()
    {
        fail("bad function call", 0);
    }

    namespace chrono
    {
        inline namespace _V2
        {
            steady_clock::time_point steady_clock::now() noexcept
            {
                timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                return time_point(duration(seconds(ts.tv_sec) + nanoseconds(ts.tv_nsec)));
            }
            system_clock::time_point system_clock::now() noexcept
            {
                timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                return time_point(duration(seconds(ts.tv_sec) + nanoseconds(ts.tv_nsec)));
            }
        }
    }
}

extern "C" {
void __cxa_pure_virtual(void)
{
    fail("pure virtual call", 0);
}
// no threads: a guard is its own first byte
int __cxa_guard_acquire(long long* guard)
{
    return !*(char*)guard;
}
void __cxa_guard_release(long long* guard)
{
    *(char*)guard = 1;
}
void __cxa_guard_abort(long long*)
{
}
void* __dso_handle;
}
