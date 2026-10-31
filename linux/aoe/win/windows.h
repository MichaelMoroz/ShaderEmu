// What the game asks of Windows, on the machine: types, messages and the calls behind win32.cpp.
// No part of Windows is here; the names are the game's way of asking for what the port gives.
#ifndef AOE_WINDOWS_H
#define AOE_WINDOWS_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>

#define WINAPI
#define APIENTRY
#define CALLBACK
#define PASCAL
#define pascal
#define __stdcall
#define __cdecl
#define _cdecl
#define cdecl
#define __fastcall
#define __inline inline
#define _inline inline
#define __forceinline inline
#define __declspec(x)
#define __int8 char
#define __int16 short
#define __int32 int
#define __int64 long long
#define near
#define far
#define NEAR
#define FAR
#define CONST const
#define IN
#define OUT
#define VOID void
#define _MAX_PATH 260
#define MAX_PATH 260
#define _MAX_FNAME 256
#define _MAX_EXT 256
#define _MAX_DIR 256
#define _MAX_DRIVE 3
#define TRUE 1
#define FALSE 0

typedef int BOOL;
typedef BOOL* LPBOOL;
typedef BOOL* PBOOL;
typedef unsigned char BYTE, UCHAR, *LPBYTE, *PBYTE, *PUCHAR;
typedef unsigned char BOOLEAN;
typedef char CHAR, *LPSTR, *PSTR, *NPSTR, TCHAR, *LPTSTR;
typedef const char *LPCSTR, *PCSTR, *LPCTSTR;
typedef unsigned short WORD, USHORT, *LPWORD, *PWORD, *PUSHORT, WCHAR, *LPWSTR;
typedef const unsigned short *LPCWSTR;
typedef short SHORT, *PSHORT;
typedef unsigned long DWORD, ULONG, *LPDWORD, *PDWORD, *PULONG;
typedef long LONG, *LPLONG, *PLONG;
typedef int INT, *LPINT, *PINT;
typedef unsigned int UINT, *LPUINT, *PUINT;
typedef float FLOAT;
typedef void *LPVOID, *PVOID, *HANDLE, *LPUNKNOWN;
typedef const void *LPCVOID;
typedef HANDLE* LPHANDLE;
typedef long LRESULT, LPARAM, HRESULT;
typedef unsigned int WPARAM;
typedef WORD ATOM;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef DWORD COLORREF, *LPCOLORREF;
typedef struct _GUID { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID, *LPGUID, IID, CLSID;
#define REFIID const GUID&
typedef const GUID* LPCGUID;
#define REFGUID const GUID&
#define REFCLSID const GUID&

typedef void* HWND; typedef void* HINSTANCE; typedef void* HDC; typedef void* HBRUSH;
typedef void* HPEN; typedef void* HFONT; typedef void* HBITMAP; typedef void* HPALETTE;
typedef void* HRGN; typedef void* HMENU; typedef void* HICON; typedef void* HKEY;
typedef void* HRSRC; typedef void* HGLOBAL; typedef void* HLOCAL; typedef void* HMIXER;
typedef void* HCURSOR;
typedef HINSTANCE HMODULE;
typedef void* HGDIOBJ;
typedef HKEY* PHKEY;
typedef DWORD HIMC;
typedef int HFILE;
typedef void* HDWP;
typedef void* HACCEL;
typedef void* FARPROC;
typedef void* HMMIO;
typedef void* HIC;
#define HFILE_ERROR (-1)
#define INVALID_HANDLE_VALUE ((HANDLE)-1)

typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT, *PPOINT, POINTL;
typedef struct tagSIZE { LONG cx, cy; } SIZE, *LPSIZE, *PSIZE;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT, *PRECT;
typedef const RECT* LPCRECT;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef struct tagPAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT, *LPPAINTSTRUCT;
typedef struct tagWNDCLASS { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCSTR lpszMenuName, lpszClassName; } WNDCLASS, WNDCLASSA, *LPWNDCLASS;
typedef struct tagPALETTEENTRY { BYTE peRed, peGreen, peBlue, peFlags; } PALETTEENTRY, *LPPALETTEENTRY, *PPALETTEENTRY;
typedef struct tagLOGPALETTE { WORD palVersion, palNumEntries; PALETTEENTRY palPalEntry[1]; } LOGPALETTE, *LPLOGPALETTE, *PLOGPALETTE;
typedef struct tagRGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD, *LPRGBQUAD;
typedef struct tagRGBTRIPLE { BYTE rgbtBlue, rgbtGreen, rgbtRed; } RGBTRIPLE;
#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct tagBITMAPINFOHEADER { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER, *LPBITMAPINFOHEADER, *PBITMAPINFOHEADER;
typedef struct tagBITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO, *LPBITMAPINFO, *PBITMAPINFO;
typedef struct tagTEXTMETRIC { LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY; BYTE tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar, tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet; } TEXTMETRIC, TEXTMETRICA, *LPTEXTMETRIC;
typedef struct tagLOGFONT { LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight; BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily; char lfFaceName[32]; } LOGFONT, *LPLOGFONT;
typedef struct tagWINDOWPOS { HWND hwnd, hwndInsertAfter; int x, y, cx, cy; UINT flags; } WINDOWPOS, *LPWINDOWPOS;
typedef struct tagCREATESTRUCT { LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent; int cy, cx, y, x; LONG style; LPCSTR lpszName, lpszClass; DWORD dwExStyle; } CREATESTRUCT, *LPCREATESTRUCT;
typedef struct tagMINMAXINFO { POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; } MINMAXINFO, *LPMINMAXINFO;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct _OSVERSIONINFO { DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId; char szCSDVersion[128]; } OSVERSIONINFO, OSVERSIONINFOA, *LPOSVERSIONINFO;
typedef struct _MEMORYSTATUS { DWORD dwLength, dwMemoryLoad, dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual; } MEMORYSTATUS, *LPMEMORYSTATUS;
typedef struct _SYSTEM_INFO { DWORD dwOemId, dwPageSize; LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress; DWORD dwActiveProcessorMask, dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO, *LPSYSTEM_INFO;
typedef struct _WIN32_FIND_DATA { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1; char cFileName[260], cAlternateFileName[14]; } WIN32_FIND_DATA, *LPWIN32_FIND_DATA;
typedef struct _CRITICAL_SECTION { int unused; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
typedef struct _OVERLAPPED { DWORD a, b, c, d; HANDLE e; } OVERLAPPED, *LPOVERLAPPED;
typedef struct _SECURITY_ATTRIBUTES { DWORD a; LPVOID b; BOOL c; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef struct tagSTARTUPINFO { DWORD cb; } STARTUPINFO, *LPSTARTUPINFO;
typedef struct _PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct tagVS_FIXEDFILEINFO { DWORD a[13]; } VS_FIXEDFILEINFO;
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(void*);
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#define LOWORD(l) ((WORD)((DWORD)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((DWORD)(l) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(w))
#define HIBYTE(w) ((BYTE)(((WORD)(w) >> 8) & 0xff))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MAKELPARAM(l, h) ((LPARAM)MAKELONG(l, h))
#define MAKEWPARAM(l, h) ((WPARAM)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)MAKELONG(l, h))
#define MAKEINTRESOURCE(i) ((LPSTR)((DWORD)((WORD)(i))))
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))
#define PALETTERGB(r, g, b) (0x02000000 | RGB(r, g, b))
#define PALETTEINDEX(i) ((COLORREF)(0x01000000 | (DWORD)(WORD)(i)))
#define SUCCEEDED(h) ((HRESULT)(h) >= 0)
#define FAILED(h) ((HRESULT)(h) < 0)
#define S_OK 0
#define S_FALSE 1
#define E_FAIL ((HRESULT)0x80004005)
#define ERROR_SUCCESS 0
#define NO_ERROR 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ALREADY_EXISTS 183
#define ZeroMemory(d, n) memset((d), 0, (n))
#define FillMemory(d, n, v) memset((d), (v), (n))
#define CopyMemory(d, s, n) memcpy((d), (s), (n))
#define MoveMemory(d, s, n) memmove((d), (s), (n))
#define DSCAPS_PRIMARY16BIT 8
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

// messages
#define WM_NULL 0
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_MOVE 3
#define WM_SIZE 5
#define WM_ACTIVATE 6
#define WM_SETFOCUS 7
#define WM_KILLFOCUS 8
#define WM_ENABLE 0x0a
#define WM_SETREDRAW 0x0b
#define WM_SETTEXT 0x0c
#define WM_GETTEXT 0x0d
#define WM_GETTEXTLENGTH 0x0e
#define WM_PAINT 0x0f
#define WM_CLOSE 0x10
#define WM_QUERYENDSESSION 0x11
#define WM_QUIT 0x12
#define WM_QUERYOPEN 0x13
#define WM_ERASEBKGND 0x14
#define WM_SYSCOLORCHANGE 0x15
#define WM_ENDSESSION 0x16
#define WM_SHOWWINDOW 0x18
#define WM_WININICHANGE 0x1a
#define WM_ACTIVATEAPP 0x1c
#define WM_SETCURSOR 0x20
#define WM_MOUSEACTIVATE 0x21
#define WM_GETMINMAXINFO 0x24
#define WM_ICONERASEBKGND 0x27
#define WM_SETFONT 0x30
#define WM_GETFONT 0x31
#define WM_WINDOWPOSCHANGING 0x46
#define WM_WINDOWPOSCHANGED 0x47
#define WM_POWER 0x48
#define WM_NCCREATE 0x81
#define WM_NCDESTROY 0x82
#define WM_NCCALCSIZE 0x83
#define WM_NCHITTEST 0x84
#define WM_NCPAINT 0x85
#define WM_NCACTIVATE 0x86
#define WM_NCMOUSEMOVE 0xa0
#define WM_NCLBUTTONDOWN 0xa1
#define WM_NCLBUTTONUP 0xa2
#define WM_NCLBUTTONDBLCLK 0xa3
#define WM_NCRBUTTONDOWN 0xa4
#define WM_KEYFIRST 0x100
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_CHAR 0x102
#define WM_DEADCHAR 0x103
#define WM_SYSKEYDOWN 0x104
#define WM_SYSKEYUP 0x105
#define WM_SYSCHAR 0x106
#define WM_KEYLAST 0x108
#define WM_IME_STARTCOMPOSITION 0x10d
#define WM_IME_ENDCOMPOSITION 0x10e
#define WM_IME_COMPOSITION 0x10f
#define WM_IME_KEYLAST 0x10f
#define WM_INITDIALOG 0x110
#define WM_COMMAND 0x111
#define WM_SYSCOMMAND 0x112
#define WM_TIMER 0x113
#define WM_HSCROLL 0x114
#define WM_VSCROLL 0x115
#define WM_INITMENU 0x116
#define WM_INITMENUPOPUP 0x117
#define WM_MENUSELECT 0x11f
#define WM_MENUCHAR 0x120
#define WM_ENTERIDLE 0x121
#define WM_CTLCOLORDLG 0x136
#define WM_CTLCOLORSTATIC 0x138
#define WM_MOUSEFIRST 0x200
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_LBUTTONDBLCLK 0x203
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_RBUTTONDBLCLK 0x206
#define WM_MBUTTONDOWN 0x207
#define WM_MBUTTONUP 0x208
#define WM_MBUTTONDBLCLK 0x209
#define WM_MOUSELAST 0x209
#define WM_ENTERSIZEMOVE 0x231
#define WM_EXITSIZEMOVE 0x232
#define WM_IME_SETCONTEXT 0x281
#define WM_IME_NOTIFY 0x282
#define WM_IME_CONTROL 0x283
#define WM_CUT 0x300
#define WM_COPY 0x301
#define WM_PASTE 0x302
#define WM_CLEAR 0x303
#define WM_UNDO 0x304
#define WM_QUERYNEWPALETTE 0x30f
#define WM_PALETTEISCHANGING 0x310
#define WM_PALETTECHANGED 0x311
#define WM_HOTKEY 0x312
#define WM_DISPLAYCHANGE 0x7e
#define WM_USER 0x400
#define WM_APP 0x8000
#define MM_MCINOTIFY 0x3b9
#define MM_WOM_DONE 0x3bd
#define MK_LBUTTON 1
#define MK_RBUTTON 2
#define MK_SHIFT 4
#define MK_CONTROL 8
#define MK_MBUTTON 0x10
#define SC_SCREENSAVE 0xf140
#define SC_MONITORPOWER 0xf170
#define SC_CLOSE 0xf060
#define SC_KEYMENU 0xf100
#define SC_MINIMIZE 0xf020
#define SC_MAXIMIZE 0xf030
#define SC_MOVE 0xf010
#define SC_SIZE 0xf000
#define SC_TASKLIST 0xf130
#define SIZE_MINIMIZED 1
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define HTCLIENT 1
#define MA_ACTIVATE 1
#define PBT_APMQUERYSUSPEND 0
#define BROADCAST_QUERY_DENY 0x424d5144
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GCL_HCURSOR (-12)
#define PM_NOREMOVE 0
#define PM_REMOVE 1
#define PM_NOYIELD 2
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SWP_NOSIZE 1
#define SWP_NOMOVE 2
#define SWP_NOZORDER 4
#define SWP_NOREDRAW 8
#define SWP_NOACTIVATE 0x10
#define SWP_SHOWWINDOW 0x40
#define SWP_HIDEWINDOW 0x80
#define HWND_TOP ((HWND)0)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)
#define HWND_BROADCAST ((HWND)0xffff)
#define WS_OVERLAPPED 0
#define WS_POPUP 0x80000000
#define WS_CHILD 0x40000000
#define WS_MINIMIZE 0x20000000
#define WS_VISIBLE 0x10000000
#define WS_DISABLED 0x8000000
#define WS_CLIPSIBLINGS 0x4000000
#define WS_CLIPCHILDREN 0x2000000
#define WS_MAXIMIZE 0x1000000
#define WS_CAPTION 0xc00000
#define WS_BORDER 0x800000
#define WS_DLGFRAME 0x400000
#define WS_VSCROLL 0x200000
#define WS_HSCROLL 0x100000
#define WS_SYSMENU 0x80000
#define WS_THICKFRAME 0x40000
#define WS_GROUP 0x20000
#define WS_TABSTOP 0x10000
#define WS_MINIMIZEBOX 0x20000
#define WS_MAXIMIZEBOX 0x10000
#define WS_OVERLAPPEDWINDOW 0xcf0000
#define WS_EX_TOPMOST 8
#define WS_EX_TRANSPARENT 0x20
#define WS_EX_APPWINDOW 0x40000
#define WS_EX_CLIENTEDGE 0x200
#define CS_VREDRAW 1
#define CS_HREDRAW 2
#define CS_DBLCLKS 8
#define CS_OWNDC 0x20
#define CS_CLASSDC 0x40
#define CS_PARENTDC 0x80
#define CW_USEDEFAULT ((int)0x80000000)
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_WAIT MAKEINTRESOURCE(32514)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)
#define COLOR_WINDOW 5
#define COLOR_BTNFACE 15
#define BLACK_BRUSH 4
#define WHITE_BRUSH 0
#define NULL_BRUSH 5
#define HOLLOW_BRUSH 5
#define NULL_PEN 8
#define BLACK_PEN 7
#define WHITE_PEN 6
#define SYSTEM_FONT 13
#define ANSI_VAR_FONT 12
#define DEFAULT_GUI_FONT 17
#define DEFAULT_PALETTE 15
#define PS_SOLID 0
#define PS_NULL 5
#define PS_DOT 2
#define TRANSPARENT 1
#define OPAQUE 2
#define SRCCOPY 0xcc0020
#define BLACKNESS 0x42
#define WHITENESS 0xff0062
#define PATCOPY 0xf00021
#define DSTINVERT 0x550009
#define SRCPAINT 0xee0086
#define SRCAND 0x8800c6
#define SRCINVERT 0x660046
#define DIB_RGB_COLORS 0
#define DIB_PAL_COLORS 1
#define BI_RGB 0
#define BI_RLE8 1
#define PC_NOCOLLAPSE 4
#define PC_RESERVED 1
#define PC_EXPLICIT 2
#define SYSPAL_STATIC 1
#define SYSPAL_NOSTATIC 2
#define HORZRES 8
#define VERTRES 10
#define BITSPIXEL 12
#define PLANES 14
#define NUMCOLORS 24
#define RASTERCAPS 38
#define SIZEPALETTE 104
#define NUMRESERVED 106
#define COLORRES 108
#define LOGPIXELSX 88
#define LOGPIXELSY 90
#define RC_PALETTE 0x100
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_MOUSEPRESENT 19
#define SM_CMOUSEBUTTONS 43
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYMENU 15
#define MB_OK 0
#define MB_OKCANCEL 1
#define MB_YESNO 4
#define MB_ICONHAND 0x10
#define MB_ICONERROR 0x10
#define MB_ICONSTOP 0x10
#define MB_ICONEXCLAMATION 0x30
#define MB_ICONQUESTION 0x20
#define MB_ICONINFORMATION 0x40
#define MB_ICONWARNING 0x30
#define MB_SYSTEMMODAL 0x1000
#define MB_APPLMODAL 0
#define MB_TOPMOST 0x40000
#define MB_SETFOREGROUND 0x10000
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
#define DT_LEFT 0
#define DT_TOP 0
#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_EXPANDTABS 0x40
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define DT_END_ELLIPSIS 0x8000
#define FW_DONTCARE 0
#define FW_NORMAL 400
#define FW_BOLD 700
#define ANSI_CHARSET 0
#define DEFAULT_CHARSET 1
#define SHIFTJIS_CHARSET 128
#define OUT_DEFAULT_PRECIS 0
#define OUT_TT_PRECIS 4
#define OUT_TT_ONLY_PRECIS 7
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define PROOF_QUALITY 2
#define DRAFT_QUALITY 1
#define ANTIALIASED_QUALITY 4
#define NONANTIALIASED_QUALITY 3
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define FF_DONTCARE 0
#define FF_ROMAN 0x10
#define FF_SWISS 0x20
#define FF_MODERN 0x30
#define TA_LEFT 0
#define TA_TOP 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define R2_COPYPEN 13
#define R2_XORPEN 7
#define R2_NOT 6
#define VK_LBUTTON 1
#define VK_RBUTTON 2
#define VK_CANCEL 3
#define VK_MBUTTON 4
#define VK_BACK 8
#define VK_TAB 9
#define VK_CLEAR 0x0c
#define VK_RETURN 0x0d
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1b
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_SNAPSHOT 0x2c
#define VK_INSERT 0x2d
#define VK_DELETE 0x2e
#define VK_HELP 0x2f
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6a
#define VK_ADD 0x6b
#define VK_SUBTRACT 0x6d
#define VK_DECIMAL 0x6e
#define VK_DIVIDE 0x6f
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7a
#define VK_F12 0x7b
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_PROCESSKEY 0xe5
#define MAPVK_VK_TO_CHAR 2
#define HKEY_CLASSES_ROOT ((HKEY)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)0x80000002)
#define KEY_ALL_ACCESS 0xf003f
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_QUERY_VALUE 1
#define KEY_SET_VALUE 2
#define REG_NONE 0
#define REG_SZ 1
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_OPTION_NON_VOLATILE 0
#define REG_CREATED_NEW_KEY 1
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define OF_READ 0
#define OF_WRITE 1
#define OF_READWRITE 2
#define OF_SHARE_DENY_NONE 0x40
#define INFINITE 0xffffffff
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define HIGH_PRIORITY_CLASS 0x80
#define NORMAL_PRIORITY_CLASS 0x20
#define VER_PLATFORM_WIN32_WINDOWS 1
#define VER_PLATFORM_WIN32_NT 2
#define GMEM_FIXED 0
#define GMEM_MOVEABLE 2
#define GMEM_ZEROINIT 0x40
#define GMEM_SHARE 0x2000
#define GHND 0x42
#define GPTR 0x40
#define RT_BITMAP MAKEINTRESOURCE(2)
#define RT_RCDATA MAKEINTRESOURCE(10)
#define SEM_NOOPENFILEERRORBOX 0x8000
#define SEM_FAILCRITICALERRORS 1
#define DRIVE_CDROM 5
#define DRIVE_FIXED 3
#define DRIVE_UNKNOWN 0
#define DRIVE_REMOVABLE 2
#define DRIVE_REMOTE 4
#define DRIVE_RAMDISK 6
#define DRIVE_NO_ROOT_DIR 1
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define LMEM_FIXED 0
#define LPTR 0x40
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100
#define LANG_NEUTRAL 0
#define SUBLANG_DEFAULT 1
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))
#define CP_ACP 0
#define HWND_DESKTOP ((HWND)0)
#define WHEEL_DELTA 120
#define ES_LEFT 0
#define ES_MULTILINE 4
#define ES_AUTOHSCROLL 0x80
#define ES_AUTOVSCROLL 0x40
#define ES_PASSWORD 0x20
#define ES_READONLY 0x800
#define ES_NUMBER 0x2000
#define ES_WANTRETURN 0x1000
#define ES_CENTER 1
#define ES_RIGHT 2
#define ES_NOHIDESEL 0x100
#define EM_SETSEL 0xb1
#define EM_GETSEL 0xb0
#define EM_REPLACESEL 0xc2
#define EM_LIMITTEXT 0xc5
#define EM_SETLIMITTEXT 0xc5
#define EM_GETLINECOUNT 0xba
#define EM_LINESCROLL 0xb6
#define EM_SETREADONLY 0xcf
#define GWW_ID (-12)
#define GWL_ID (-12)
#define GCW_HCURSOR (-12)

#ifdef __cplusplus
extern "C" {
#endif
// the clock and the machine
DWORD GetTickCount(void);
DWORD timeGetTime(void);
void Sleep(DWORD);
BOOL QueryPerformanceCounter(void*);
BOOL QueryPerformanceFrequency(void*);
void GetLocalTime(LPSYSTEMTIME);
void GetSystemTime(LPSYSTEMTIME);
void GlobalMemoryStatus(LPMEMORYSTATUS);
void GetSystemInfo(LPSYSTEM_INFO);
BOOL GetVersionEx(LPOSVERSIONINFO);
DWORD GetVersion(void);
DWORD GetLastError(void);
void SetLastError(DWORD);
HANDLE GetCurrentProcess(void);
HANDLE GetCurrentThread(void);
DWORD GetCurrentProcessId(void);
DWORD GetCurrentThreadId(void);
BOOL SetPriorityClass(HANDLE, DWORD);
BOOL SetThreadPriority(HANDLE, int);
UINT SetErrorMode(UINT);
void OutputDebugString(LPCSTR);
BOOL IsBadReadPtr(const void*, UINT);
BOOL IsBadWritePtr(void*, UINT);
void ExitProcess(UINT);
LPSTR GetCommandLine(void);
void InitializeCriticalSection(LPCRITICAL_SECTION);
void EnterCriticalSection(LPCRITICAL_SECTION);
void LeaveCriticalSection(LPCRITICAL_SECTION);
void DeleteCriticalSection(LPCRITICAL_SECTION);
HANDLE CreateEvent(void*, BOOL, BOOL, LPCSTR);
BOOL SetEvent(HANDLE);
BOOL ResetEvent(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
DWORD WaitForMultipleObjects(DWORD, const HANDLE*, BOOL, DWORD);
BOOL CloseHandle(HANDLE);
HANDLE CreateMutex(void*, BOOL, LPCSTR);
BOOL ReleaseMutex(HANDLE);
HANDLE CreateThread(void*, DWORD, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
BOOL TerminateThread(HANDLE, DWORD);
// modules, resources, strings
HMODULE GetModuleHandle(LPCSTR);
DWORD GetModuleFileName(HMODULE, LPSTR, DWORD);
HINSTANCE LoadLibrary(LPCSTR);
BOOL FreeLibrary(HMODULE);
FARPROC GetProcAddress(HMODULE, LPCSTR);
int LoadString(HINSTANCE, UINT, LPSTR, int);
HICON LoadIcon(HINSTANCE, LPCSTR);
HCURSOR LoadCursor(HINSTANCE, LPCSTR);
HBITMAP LoadBitmap(HINSTANCE, LPCSTR);
HRSRC FindResource(HMODULE, LPCSTR, LPCSTR);
HGLOBAL LoadResource(HMODULE, HRSRC);
LPVOID LockResource(HGLOBAL);
DWORD SizeofResource(HMODULE, HRSRC);
BOOL FreeResource(HGLOBAL);
HGLOBAL GlobalAlloc(UINT, DWORD);
HGLOBAL GlobalFree(HGLOBAL);
LPVOID GlobalLock(HGLOBAL);
BOOL GlobalUnlock(HGLOBAL);
DWORD GlobalSize(HGLOBAL);
HGLOBAL LocalAlloc(UINT, DWORD);
HGLOBAL LocalFree(HGLOBAL);
LPVOID VirtualAlloc(LPVOID, DWORD, DWORD, DWORD);
BOOL VirtualFree(LPVOID, DWORD, DWORD);
// windows and messages
ATOM RegisterClass(const WNDCLASS*);
BOOL UnregisterClass(LPCSTR, HINSTANCE);
HWND CreateWindowEx(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
#define CreateWindow(c, n, s, x, y, w, h, p, m, i, l) CreateWindowEx(0, c, n, s, x, y, w, h, p, m, i, l)
BOOL DestroyWindow(HWND);
LRESULT DefWindowProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CallWindowProc(WNDPROC, HWND, UINT, WPARAM, LPARAM);
BOOL ShowWindow(HWND, int);
BOOL UpdateWindow(HWND);
BOOL IsWindow(HWND);
BOOL IsWindowVisible(HWND);
BOOL IsIconic(HWND);
BOOL IsZoomed(HWND);
BOOL EnableWindow(HWND, BOOL);
BOOL IsWindowEnabled(HWND);
BOOL MoveWindow(HWND, int, int, int, int, BOOL);
BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT);
HWND SetFocus(HWND);
HWND GetFocus(void);
HWND GetActiveWindow(void);
HWND SetActiveWindow(HWND);
HWND GetForegroundWindow(void);
BOOL SetForegroundWindow(HWND);
HWND GetDesktopWindow(void);
HWND GetParent(HWND);
HWND SetParent(HWND, HWND);
HWND GetTopWindow(HWND);
HWND FindWindow(LPCSTR, LPCSTR);
HWND SetCapture(HWND);
HWND GetCapture(void);
BOOL ReleaseCapture(void);
LONG GetWindowLong(HWND, int);
LONG SetWindowLong(HWND, int, LONG);
DWORD GetClassLong(HWND, int);
DWORD SetClassLong(HWND, int, LONG);
BOOL GetClientRect(HWND, LPRECT);
BOOL GetWindowRect(HWND, LPRECT);
BOOL ClientToScreen(HWND, LPPOINT);
BOOL ScreenToClient(HWND, LPPOINT);
BOOL AdjustWindowRect(LPRECT, DWORD, BOOL);
BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD);
BOOL InvalidateRect(HWND, const RECT*, BOOL);
BOOL ValidateRect(HWND, const RECT*);
BOOL GetUpdateRect(HWND, LPRECT, BOOL);
HDC BeginPaint(HWND, LPPAINTSTRUCT);
BOOL EndPaint(HWND, const PAINTSTRUCT*);
BOOL SetWindowText(HWND, LPCSTR);
int GetWindowText(HWND, LPSTR, int);
int GetWindowTextLength(HWND);
int GetClassName(HWND, LPSTR, int);
BOOL PeekMessage(LPMSG, HWND, UINT, UINT, UINT);
BOOL GetMessage(LPMSG, HWND, UINT, UINT);
BOOL TranslateMessage(const MSG*);
LRESULT DispatchMessage(const MSG*);
void PostQuitMessage(int);
BOOL PostMessage(HWND, UINT, WPARAM, LPARAM);
LRESULT SendMessage(HWND, UINT, WPARAM, LPARAM);
BOOL WaitMessage(void);
DWORD GetMessagePos(void);
LONG GetMessageTime(void);

// GDI: a device context is a surface and what it draws with (gdi.cpp)
#define BI_BITFIELDS 3
#define OF_EXIST 0x4000
#define MAKEINTRESOURCEA MAKEINTRESOURCE
typedef struct _OFSTRUCT { BYTE cBytes, fFixedDisk; WORD nErrCode, r1, r2; char szPathName[128]; } OFSTRUCT, *LPOFSTRUCT;
HDC GetDC(HWND);
int ReleaseDC(HWND, HDC);
HDC CreateCompatibleDC(HDC);
BOOL DeleteDC(HDC);
HGDIOBJ SelectObject(HDC, HGDIOBJ);
BOOL DeleteObject(HGDIOBJ);
HGDIOBJ GetStockObject(int);
int GetObject(HGDIOBJ, int, LPVOID);
int GetDeviceCaps(HDC, int);
int SetBkMode(HDC, int);
COLORREF SetBkColor(HDC, COLORREF);
COLORREF SetTextColor(HDC, COLORREF);
UINT SetTextAlign(HDC, UINT);
UINT GetTextAlign(HDC);
BOOL TextOutA(HDC, int, int, LPCSTR, int);
#define TextOut TextOutA
BOOL ExtTextOutA(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const int*);
#define ExtTextOut ExtTextOutA
int DrawTextA(HDC, LPCSTR, int, LPRECT, UINT);
#define DrawText DrawTextA
BOOL GetTextExtentPoint32A(HDC, LPCSTR, int, LPSIZE);
BOOL GetTextExtentPointA(HDC, LPCSTR, int, LPSIZE);
#define GetTextExtentPoint32 GetTextExtentPoint32A
#define GetTextExtentPoint GetTextExtentPointA
BOOL GetTextMetricsA(HDC, LPTEXTMETRIC);
#define GetTextMetrics GetTextMetricsA
HFONT CreateFontA(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);
#define CreateFont CreateFontA
HFONT CreateFontIndirectA(const LOGFONT*);
#define CreateFontIndirect CreateFontIndirectA
int AddFontResourceA(LPCSTR);
BOOL RemoveFontResourceA(LPCSTR);
#define AddFontResource AddFontResourceA
#define RemoveFontResource RemoveFontResourceA
HPEN CreatePen(int, int, COLORREF);
HBRUSH CreateSolidBrush(COLORREF);
HRGN CreateRectRgn(int, int, int, int);
HRGN CreateRectRgnIndirect(const RECT*);
int SelectClipRgn(HDC, HRGN);
int CombineRgn(HRGN, HRGN, HRGN, int);
int GetRgnBox(HRGN, LPRECT);
BOOL SetRectRgn(HRGN, int, int, int, int);
BOOL MoveToEx(HDC, int, int, LPPOINT);
BOOL LineTo(HDC, int, int);
BOOL Rectangle(HDC, int, int, int, int);
int FillRect(HDC, const RECT*, HBRUSH);
BOOL PatBlt(HDC, int, int, int, int, DWORD);
BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
BOOL StretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
int StretchDIBits(HDC, int, int, int, int, int, int, int, int, const void*, const BITMAPINFO*, UINT, DWORD);
int SetDIBitsToDevice(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void*, const BITMAPINFO*, UINT);
HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
HBITMAP CreateCompatibleBitmap(HDC, int, int);
UINT SetDIBColorTable(HDC, UINT, UINT, const RGBQUAD*);
UINT GetDIBColorTable(HDC, UINT, UINT, RGBQUAD*);
HPALETTE CreatePalette(const LOGPALETTE*);
HPALETTE SelectPalette(HDC, HPALETTE, BOOL);
UINT RealizePalette(HDC);
UINT SetPaletteEntries(HPALETTE, UINT, UINT, const PALETTEENTRY*);
UINT GetPaletteEntries(HPALETTE, UINT, UINT, LPPALETTEENTRY);
UINT GetSystemPaletteEntries(HDC, UINT, UINT, LPPALETTEENTRY);
UINT GetSystemPaletteUse(HDC);
UINT SetSystemPaletteUse(HDC, UINT);
BOOL AnimatePalette(HPALETTE, UINT, UINT, const PALETTEENTRY*);
UINT GetNearestPaletteIndex(HPALETTE, COLORREF);
COLORREF GetPixel(HDC, int, int);
COLORREF SetPixel(HDC, int, int, COLORREF);
int SetROP2(HDC, int);
BOOL GdiFlush(void);
BOOL SetRect(LPRECT, int, int, int, int);
BOOL SetRectEmpty(LPRECT);
BOOL IsRectEmpty(const RECT*);
BOOL InflateRect(LPRECT, int, int);
BOOL OffsetRect(LPRECT, int, int);
BOOL IntersectRect(LPRECT, const RECT*, const RECT*);
BOOL UnionRect(LPRECT, const RECT*, const RECT*);
BOOL PtInRect(const RECT*, POINT);
BOOL CopyRect(LPRECT, const RECT*);
// input and the rest of the window manager
int MessageBoxA(HWND, LPCSTR, LPCSTR, UINT);
#define MessageBox MessageBoxA
BOOL MessageBeep(UINT);
int ShowCursor(BOOL);
HCURSOR SetCursor(HCURSOR);
HCURSOR GetCursor(void);
BOOL SetCursorPos(int, int);
BOOL GetCursorPos(LPPOINT);
BOOL ClipCursor(const RECT*);
SHORT GetKeyState(int);
SHORT GetAsyncKeyState(int);
BOOL GetKeyboardState(LPBYTE);
int ToAscii(UINT, UINT, const BYTE*, LPWORD, UINT);
UINT MapVirtualKey(UINT, UINT);
int GetSystemMetrics(int);
BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT);
#define SystemParametersInfo SystemParametersInfoA
BOOL WinHelpA(HWND, LPCSTR, UINT, DWORD);
#define WinHelp WinHelpA
#define HELP_CONTEXT 1
#define HELP_QUIT 2
#define HELP_INDEX 3
#define HELP_FINDER 11
#define HELP_CONTENTS 3
UINT SetTimer(HWND, UINT, UINT, void*);
BOOL KillTimer(HWND, UINT);
HMENU GetSystemMenu(HWND, BOOL);
BOOL DeleteMenu(HMENU, UINT, UINT);
BOOL DrawMenuBar(HWND);
BOOL EnumWindows(void*, LPARAM);
HWND GetWindow(HWND, UINT);
HWND GetDlgItem(HWND, int);
BOOL ImmSetOpenStatus(HIMC, BOOL);
HIMC ImmGetContext(HWND);
BOOL ImmReleaseContext(HWND, HIMC);
HIMC ImmAssociateContext(HWND, HIMC);
LONG GetBitmapBits(HBITMAP, LONG, LPVOID);
#define HWND_MESSAGE ((HWND)-3)
// the registry, a key of the image's (registry.cpp)
LONG RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, PHKEY);
LONG RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, DWORD, void*, PHKEY, LPDWORD);
LONG RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
LONG RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
LONG RegCloseKey(HKEY);
LONG RegDeleteKeyA(HKEY, LPCSTR);
LONG RegDeleteValueA(HKEY, LPCSTR);
#define RegOpenKeyEx RegOpenKeyExA
#define RegCreateKeyEx RegCreateKeyExA
#define RegQueryValueEx RegQueryValueExA
#define RegSetValueEx RegSetValueExA
// files
DWORD GetTempPathA(DWORD, LPSTR);
DWORD GetCurrentDirectoryA(DWORD, LPSTR);
BOOL SetCurrentDirectoryA(LPCSTR);
UINT GetDriveTypeA(LPCSTR);
DWORD GetLogicalDrives(void);
DWORD GetLogicalDriveStringsA(DWORD, LPSTR);
BOOL GetVolumeInformationA(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD);
BOOL GetDiskFreeSpaceA(LPCSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD);
BOOL DeleteFileA(LPCSTR);
BOOL CopyFileA(LPCSTR, LPCSTR, BOOL);
BOOL CreateDirectoryA(LPCSTR, void*);
DWORD GetFileAttributesA(LPCSTR);
HANDLE CreateFileA(LPCSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, void*);
BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, void*);
DWORD SetFilePointer(HANDLE, LONG, LPLONG, DWORD);
DWORD GetFileSize(HANDLE, LPDWORD);
HANDLE FindFirstFileA(LPCSTR, LPWIN32_FIND_DATA);
BOOL FindNextFileA(HANDLE, LPWIN32_FIND_DATA);
BOOL FindClose(HANDLE);
#define GetTempPath GetTempPathA
#define GetCurrentDirectory GetCurrentDirectoryA
#define SetCurrentDirectory SetCurrentDirectoryA
#define GetDriveType GetDriveTypeA
#define DeleteFile DeleteFileA
#define CreateFile CreateFileA
HFILE OpenFile(LPCSTR, LPOFSTRUCT, UINT);
HFILE _lopen(LPCSTR, int);
HFILE _lcreat(LPCSTR, int);
HFILE _lclose(HFILE);
UINT _lread(HFILE, LPVOID, UINT);
UINT _lwrite(HFILE, LPCSTR, UINT);
LONG _llseek(HFILE, LONG, int);
LONG _hread(HFILE, LPVOID, LONG);
LONG _hwrite(HFILE, LPCSTR, LONG);
HGLOBAL GlobalReAlloc(HGLOBAL, DWORD, UINT);
HGLOBAL GlobalHandle(LPCVOID);
HANDLE GetStdHandle(DWORD);
int WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, LPBOOL);
int MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
int lstrlenA(LPCSTR);
char* lstrcpyA(LPSTR, LPCSTR);
char* lstrcatA(LPSTR, LPCSTR);
int lstrcmpA(LPCSTR, LPCSTR);
int lstrcmpiA(LPCSTR, LPCSTR);
#define lstrlen lstrlenA
#define lstrcpy lstrcpyA
#define lstrcat lstrcatA
#define lstrcmp lstrcmpA
#define lstrcmpi lstrcmpiA
int wsprintfA(LPSTR, LPCSTR, ...);
#define wsprintf wsprintfA
// the C library of Visual C++
char* itoa(int, char*, int);
char* ltoa(long, char*, int);
char* ultoa(unsigned long, char*, int);
char* strupr(char*);
char* strlwr(char*);
#define _itoa itoa
#define _ltoa ltoa
#define _ultoa ultoa
#define _strupr strupr
#define _strlwr strlwr
#define stricmp strcasecmp
#define strcmpi strcasecmp
#define _stricmp strcasecmp
#define strnicmp strncasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _getpid getpid
void _splitpath(const char*, char*, char*, char*, char*);
void _makepath(char*, const char*, const char*, const char*, const char*);
static inline unsigned _rotl(unsigned v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline unsigned _rotr(unsigned v, int n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
long _ftol_(double);
// mixer: the volume is the sound card's (dsound.cpp)
typedef struct { DWORD cbStruct, dwDestination, dwSource, dwLineID, fdwLine, dwUser, dwComponentType, cChannels, cConnections, cControls; char szShortName[16], szName[64]; struct { DWORD dwType, dwDeviceID; WORD wMid, wPid; DWORD vDriverVersion; char szPname[32]; } Target; } MIXERLINEA, MIXERLINE, *LPMIXERLINE;
typedef struct { DWORD cbStruct, dwControlID, dwControlType, fdwControl, cMultipleItems; char szShortName[16], szName[64]; union { struct { LONG lMinimum, lMaximum; }; struct { DWORD dwMinimum, dwMaximum; }; DWORD r[6]; } Bounds; union { DWORD cSteps, cbCustomData; DWORD r[6]; } Metrics; } MIXERCONTROLA, MIXERCONTROL, *LPMIXERCONTROL;
typedef struct { DWORD cbStruct, dwLineID; union { DWORD dwControlID, dwControlType; }; DWORD cControls, cbmxctrl; MIXERCONTROLA* pamxctrl; } MIXERLINECONTROLSA, MIXERLINECONTROLS;
typedef struct { DWORD cbStruct, dwControlID, cChannels; union { HWND hwndOwner; DWORD cMultipleItems; }; DWORD cbDetails; LPVOID paDetails; } MIXERCONTROLDETAILS, *LPMIXERCONTROLDETAILS;
typedef struct { DWORD dwValue; } MIXERCONTROLDETAILS_UNSIGNED;
#define MIXERLINE_COMPONENTTYPE_DST_SPEAKERS 4
#define MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT 0x1008
#define MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER 0x1004
#define MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC 0x1005
#define MIXERCONTROL_CONTROLTYPE_VOLUME 0x50030001
#define MIXER_GETLINEINFOF_COMPONENTTYPE 3
#define MIXER_GETLINEINFOF_SOURCE 1
#define MIXER_GETLINEINFOF_DESTINATION 0
#define MIXER_GETLINECONTROLSF_ONEBYTYPE 2
#define MIXER_GETCONTROLDETAILSF_VALUE 0
#define MIXER_SETCONTROLDETAILSF_VALUE 0
#define MMSYSERR_NOERROR 0
UINT mixerOpen(HMIXER*, UINT, DWORD, DWORD, DWORD);
UINT mixerClose(HMIXER);
UINT mixerGetLineInfoA(HMIXER, MIXERLINEA*, DWORD);
UINT mixerGetLineControlsA(HMIXER, MIXERLINECONTROLSA*, DWORD);
UINT mixerGetControlDetailsA(HMIXER, MIXERCONTROLDETAILS*, DWORD);
UINT mixerSetControlDetails(HMIXER, MIXERCONTROLDETAILS*, DWORD);
UINT mixerGetNumDevs(void);
#define mixerGetLineInfo mixerGetLineInfoA
#define mixerGetLineControls mixerGetLineControlsA
#define mixerGetControlDetails mixerGetControlDetailsA
UINT timeBeginPeriod(UINT);
UINT timeEndPeriod(UINT);
#define PostMessageA PostMessage
#define SendMessageA SendMessage
#define PeekMessageA PeekMessage
#define GetMessageA GetMessage
#define DispatchMessageA DispatchMessage
#define DefWindowProcA DefWindowProc
#define CallWindowProcA CallWindowProc
#define RegisterClassA RegisterClass
#define UnregisterClassA UnregisterClass
#define CreateWindowExA CreateWindowEx
#define LoadStringA LoadString
#define LoadIconA LoadIcon
#define LoadCursorA LoadCursor
#define LoadBitmapA LoadBitmap
#define LoadLibraryA LoadLibrary
#define GetModuleHandleA GetModuleHandle
#define GetModuleFileNameA GetModuleFileName
#define FindResourceA FindResource
#define OutputDebugStringA OutputDebugString
#define FindWindowA FindWindow
#define SetWindowTextA SetWindowText
#define GetWindowTextA GetWindowText
#define GetWindowTextLengthA GetWindowTextLength
#define GetVersionExA GetVersionEx
#define GetClassNameA GetClassName
#define CreateEventA CreateEvent
#define CreateMutexA CreateMutex
#define GetCommandLineA GetCommandLine
#define GetWindowLongA GetWindowLong
#define SetWindowLongA SetWindowLong
#define GetClassLongA GetClassLong
#define SetClassLongA SetClassLong
#define MapVirtualKeyA MapVirtualKey
#define RegisterWindowMessageA RegisterWindowMessage
UINT RegisterWindowMessage(LPCSTR);
#define OF_CREATE 0x1000
#define BI_RLE4 2
#define LMEM_ZEROINIT 0x40
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 4
#define HELP_KEY 0x101
#define LOGFONTA LOGFONT
#define E_POINTER ((HRESULT)0x80004003)
#define E_PENDING ((HRESULT)0x8000000a)
#define E_OUTOFMEMORY ((HRESULT)0x8007000e)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_HANDLE ((HRESULT)0x80070006)
#define E_ACCESSDENIED ((HRESULT)0x80070005)
#define E_ABORT ((HRESULT)0x80004004)
#define CLASS_E_NOAGGREGATION ((HRESULT)0x80040110)
#define HMIXEROBJ HMIXER
typedef struct tagBITMAP { LONG bmType, bmWidth, bmHeight, bmWidthBytes; WORD bmPlanes, bmBitsPixel; LPVOID bmBits; } BITMAP, *LPBITMAP;
typedef struct tagBITMAPCOREHEADER { DWORD bcSize; WORD bcWidth, bcHeight, bcPlanes, bcBitCount; } BITMAPCOREHEADER;
int MulDiv(int, int, int);
HWND GetLastActivePopup(HWND);
BOOL BringWindowToTop(HWND);
DWORD GetWindowThreadProcessId(HWND, LPDWORD);
UINT GetTempFileNameA(LPCSTR, LPCSTR, UINT, LPSTR);
int GetDIBits(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT);
HPALETTE CreateHalftonePalette(HDC);
int DrawTextExA(HDC, LPSTR, int, LPRECT, UINT, void*);
UINT GetCaretBlinkTime(void);
BOOL OpenClipboard(HWND);
BOOL CloseClipboard(void);
BOOL IsClipboardFormatAvailable(UINT);
HANDLE GetClipboardData(UINT);
BOOL ImmNotifyIME(HIMC, DWORD, DWORD, DWORD);
#define NI_COMPOSITIONSTR 0x15
#define CPS_CANCEL 4

#define SPIF_SENDCHANGE 2
#define SPI_SETSCREENSAVEACTIVE 17
#define SPI_GETSCREENSAVEACTIVE 16
#define SPI_SETLOWPOWERACTIVE 85
#define SPI_GETLOWPOWERACTIVE 83
#define SPI_GETPOWEROFFACTIVE 84
#define SPI_SETPOWEROFFACTIVE 86
#define NORM_IGNORECASE 1
#define LOCALE_USER_DEFAULT 0x400
#define E_INVALIDARG ((HRESULT)0x80070057)
#define COLOR_WINDOWTEXT 8
#define COLOR_BTNTEXT 18
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define EM_UNDO 0xc7
#define CF_TEXT 1
#define WM_POWERBROADCAST 0x218
#define WM_EXITMENULOOP 0x212
#define WM_ENTERMENULOOP 0x211
#define WIN32_FIND_DATAA WIN32_FIND_DATA
#define STRETCH_HALFTONE 4
#define SM_CXHSCROLL 21
#define SM_CYHSCROLL 3
#define SM_CXVSCROLL 2
LPSTR CharUpperA(LPSTR);
int CompareStringA(DWORD, DWORD, LPCSTR, int, LPCSTR, int);
#define CompareString CompareStringA
DWORD GetSysColor(int);
BOOL SetSysColors(int, const int*, const COLORREF*);
BOOL IsDBCSLeadByte(BYTE);
#define GetObjectA GetObject
BOOL FileTimeToSystemTime(const FILETIME*, LPSYSTEMTIME);
BOOL FileTimeToLocalFileTime(const FILETIME*, LPFILETIME);
HDC CreateICA(LPCSTR, LPCSTR, LPCSTR, const void*);
UINT WinExec(LPCSTR, UINT);
BOOL UnmapViewOfFile(LPCVOID);
LPVOID MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, DWORD);
HANDLE CreateFileMappingA(HANDLE, void*, DWORD, DWORD, DWORD, LPCSTR);
#define PAGE_READONLY 2
#define FILE_MAP_READ 4
int SetStretchBltMode(HDC, int);
BOOL ResizePalette(HPALETTE, UINT);
extern char* _pgmptr;

#define tagMCI_STATUS_PARMS MCI_STATUS_PARMS_T
#ifdef __cplusplus
}
// time_t is 64 bits in musl and the game's are long: time in its own, every use a long
static inline long aoe_time(long* p) { long t = (long)time((time_t*)0); if (p) *p = t; return t; }
static inline char* aoe_ctime(const long* p) { time_t t = *p; return ctime(&t); }
static inline struct tm* aoe_localtime(const long* p) { time_t t = *p; return localtime(&t); }
static inline struct tm* aoe_gmtime(const long* p) { time_t t = *p; return gmtime(&t); }
static inline long aoe_mktime(struct tm* m) { return (long)mktime(m); }
#define time aoe_time
#define ctime aoe_ctime
#define localtime aoe_localtime
#define gmtime aoe_gmtime
#define mktime aoe_mktime
#define time_t long
static const GUID GUID_NULL = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
#endif
#endif
