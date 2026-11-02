/* SPDX-License-Identifier: GPL-3.0-or-later
   ShaderEmu: the three Microsoft words of the brain's dllinterface.h and dllinterface.cpp, for
   GCC, which has none of them off Windows (OpenCNC3D builds the brain with Clang's -fdeclspec on
   macOS and with MinGW on Windows). Force-included into every C++ file of the brain by
   linux/opencnc3d/build.sh, with -fpermissive for the header's MSVC forms (a qualified
   constructor name, structs named inside an anonymous union). */
#ifndef SHADEREMU_MSVC_GCC_H
#define SHADEREMU_MSVC_GCC_H
#if !defined(_WIN32)
#define __int64 long long
#define __cdecl
#define __declspec(x)
#endif
#endif
