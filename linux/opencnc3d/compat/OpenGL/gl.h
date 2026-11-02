/* SPDX-License-Identifier: GPL-3.0-or-later
   ShaderEmu: the OpenGL header OpenCNC3D's renderer asks for by Apple's name (as its
   compat/win/OpenGL/gl.h does on Windows). The declarations are SDL2's copy of Mesa's gl.h and
   glext.h (include/SDL_opengl.h, zlib licence), with every entry point past 1.1 declared: the
   machine's GL is linux/opencnc3d/gl_ocnc.c over Half-Life's shim, linked, not fetched. */
#ifndef SHADEREMU_OCNC_GL_H
#define SHADEREMU_OCNC_GL_H
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <SDL_opengl.h>
#endif
