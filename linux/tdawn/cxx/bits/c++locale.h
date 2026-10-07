// Stands in for the toolchain's bits/c++locale.h, which is written for glibc's locale types
// (musl has none of them). This is libstdc++'s "generic" locale model, cut to what is used.
#ifndef _GLIBCXX_CXX_LOCALE_H
#define _GLIBCXX_CXX_LOCALE_H 1

#pragma GCC system_header

#include <clocale>
#include <cstdarg>
#include <cstdio>

#define _GLIBCXX_NUM_CATEGORIES 0

namespace std _GLIBCXX_VISIBILITY(default)
{
    typedef int* __c_locale;

    inline int __convert_from_v(const __c_locale&, char* __out, const int __size, const char* __fmt, ...)
    {
        __builtin_va_list __args;
        __builtin_va_start(__args, __fmt);
        const int __ret = __builtin_vsnprintf(__out, __size, __fmt, __args);
        __builtin_va_end(__args);
        return __ret;
    }
}

#endif
