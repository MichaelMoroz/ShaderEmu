// DirectDraw as the game uses it, over the machine's memory: ddraw.cpp is the whole of it.
#ifndef AOE_DDRAW_H
#define AOE_DDRAW_H
#include "com.h"
#define DD_OK 0
#define DDERR(n) ((HRESULT)(0x88760000 | (n)))
#define DDERR_GENERIC E_FAIL
#define DDERR_SURFACELOST DDERR(450)
#define DDERR_WASSTILLDRAWING DDERR(540)
#define DDERR_NOCLIPPERATTACHED DDERR(205)
#define DDERR_SURFACEBUSY DDERR(430)
#define DDSCL_FULLSCREEN 1
#define DDSCL_ALLOWREBOOT 2
#define DDSCL_NOWINDOWCHANGES 4
#define DDSCL_NORMAL 8
#define DDSCL_EXCLUSIVE 0x10
#define DDSD_CAPS 1
#define DDSD_HEIGHT 2
#define DDSD_WIDTH 4
#define DDSD_PITCH 8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSCAPS_BACKBUFFER 4
#define DDSCAPS_COMPLEX 8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDLOCK_SURFACEMEMORYPTR 0
#define DDLOCK_WAIT 1
#define DDBLT_COLORFILL 0x400
#define DDBLT_KEYSRC 0x8000
#define DDBLT_WAIT 0x1000000
#define DDBLTFAST_NOCOLORKEY 0
#define DDBLTFAST_SRCCOLORKEY 1
#define DDBLTFAST_WAIT 0x10
#define DDFLIP_WAIT 1
#define DDCKEY_SRCBLT 8
#define DDCKEY_SRCOVERLAY 0x10
#define DDPCAPS_8BIT 4
#define DDPCAPS_ALLOW256 0x40
#define DDWAITVB_BLOCKBEGIN 1
#define DDENUMRET_OK 1
#define DDENUMRET_CANCEL 0
typedef struct _DDSCAPS { DWORD dwCaps; } DDSCAPS, *LPDDSCAPS;
typedef struct _DDCOLORKEY { DWORD dwColorSpaceLowValue, dwColorSpaceHighValue; } DDCOLORKEY, *LPDDCOLORKEY;
typedef struct _DDPIXELFORMAT { DWORD dwSize, dwFlags, dwFourCC, dwRGBBitCount, dwRBitMask, dwGBitMask, dwBBitMask, dwRGBAlphaBitMask; } DDPIXELFORMAT;
typedef struct _DDSURFACEDESC { DWORD dwSize, dwFlags, dwHeight, dwWidth; LONG lPitch; DWORD dwBackBufferCount, dwMipMapCount, dwAlphaBitDepth, dwReserved; LPVOID lpSurface; DDCOLORKEY ddckCKDestOverlay, ddckCKDestBlt, ddckCKSrcOverlay, ddckCKSrcBlt; DDPIXELFORMAT ddpfPixelFormat; DDSCAPS ddsCaps; } DDSURFACEDESC, *LPDDSURFACEDESC;
typedef struct _DDBLTFX { DWORD dwSize, dwDDFX, dwROP, dwDDROP, dwRotationAngle, dwZBufferOpCode, dwZBufferLow, dwZBufferHigh, dwZBufferBaseDest, dwZDestConstBitDepth, dwZDestConst, dwZSrcConstBitDepth, dwZSrcConst, dwAlphaEdgeBlendBitDepth, dwAlphaEdgeBlend, dwReserved, dwAlphaDestConstBitDepth, dwAlphaDestConst, dwAlphaSrcConstBitDepth, dwAlphaSrcConst, dwFillColor; DDCOLORKEY ddckDestColorkey, ddckSrcColorkey; } DDBLTFX, *LPDDBLTFX;
typedef struct _DDCAPS { DWORD dwSize, dwCaps, dwCaps2, dwCKeyCaps, dwFXCaps, dwFXAlphaCaps, dwPalCaps, dwSVCaps, dwVidMemTotal, dwVidMemFree; DWORD rest[80]; DDSCAPS ddsCaps; } DDCAPS, *LPDDCAPS;
#define DDCAPS_BLTCOLORFILL 0x4000000
struct IDirectDrawPalette : IUnknown {
    PALETTEENTRY entries[256];
    int is_primary;
    virtual HRESULT SetEntries(DWORD, DWORD, DWORD, LPPALETTEENTRY);
    virtual HRESULT GetEntries(DWORD, DWORD, DWORD, LPPALETTEENTRY);
};
typedef IDirectDrawPalette* LPDIRECTDRAWPALETTE;
struct IDirectDrawClipper : IUnknown { virtual HRESULT SetHWnd(DWORD, HWND) { return DD_OK; } };
typedef IDirectDrawClipper* LPDIRECTDRAWCLIPPER;
struct IDirectDrawSurface : IUnknown {
    unsigned char* bits; int w, h, pitch, is_primary, has_key; DWORD key; void* gdi;
    virtual ULONG Release();
    virtual HRESULT Lock(LPRECT, LPDDSURFACEDESC, DWORD, HANDLE);
    virtual HRESULT Unlock(LPVOID);
    virtual HRESULT Blt(LPRECT, IDirectDrawSurface*, LPRECT, DWORD, LPDDBLTFX);
    virtual HRESULT BltFast(DWORD, DWORD, IDirectDrawSurface*, LPRECT, DWORD);
    virtual HRESULT Flip(IDirectDrawSurface*, DWORD);
    virtual HRESULT GetAttachedSurface(LPDDSCAPS, IDirectDrawSurface**);
    virtual HRESULT GetDC(HDC*);
    virtual HRESULT ReleaseDC(HDC);
    virtual HRESULT GetSurfaceDesc(LPDDSURFACEDESC);
    virtual HRESULT GetCaps(LPDDSCAPS);
    virtual HRESULT IsLost() { return DD_OK; }
    virtual HRESULT Restore() { return DD_OK; }
    virtual HRESULT SetPalette(IDirectDrawPalette*);
    virtual HRESULT SetColorKey(DWORD, LPDDCOLORKEY);
    virtual HRESULT SetClipper(IDirectDrawClipper*) { return DD_OK; }
    virtual HRESULT GetClipper(IDirectDrawClipper**) { return DDERR_NOCLIPPERATTACHED; }
    virtual HRESULT GetBltStatus(DWORD) { return DD_OK; }
    virtual HRESULT GetFlipStatus(DWORD) { return DD_OK; }
};
typedef IDirectDrawSurface* LPDIRECTDRAWSURFACE;
struct IDirectDraw : IUnknown {
    virtual HRESULT SetCooperativeLevel(HWND, DWORD);
    virtual HRESULT SetDisplayMode(DWORD, DWORD, DWORD);
    virtual HRESULT RestoreDisplayMode();
    virtual HRESULT GetDisplayMode(LPDDSURFACEDESC);
    virtual HRESULT FlipToGDISurface() { return DD_OK; }
    virtual HRESULT EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID, long (*)(_DDSURFACEDESC*, void*));
    virtual HRESULT CreateSurface(LPDDSURFACEDESC, IDirectDrawSurface**, IUnknown*);
    virtual HRESULT CreatePalette(DWORD, LPPALETTEENTRY, IDirectDrawPalette**, IUnknown*);
    virtual HRESULT CreateClipper(DWORD, IDirectDrawClipper**, IUnknown*);
    virtual HRESULT GetCaps(LPDDCAPS, LPDDCAPS);
    virtual HRESULT WaitForVerticalBlank(DWORD, HANDLE) { return DD_OK; }
};
typedef IDirectDraw* LPDIRECTDRAW;
#define DirectDrawCreate aoe_DirectDrawCreate
HRESULT aoe_DirectDrawCreate(GUID*, IDirectDraw**, IUnknown*);
#endif
