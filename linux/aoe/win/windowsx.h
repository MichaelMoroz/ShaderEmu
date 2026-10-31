#define GlobalAllocPtr(f, n) ((void*)GlobalAlloc((f), (n)))
#define GlobalFreePtr(p) GlobalFree((HGLOBAL)(p))
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))
