#ifndef AOE_COM_H
#define AOE_COM_H
#define interface struct
#define PURE = 0
#define STDMETHOD(m) virtual HRESULT m
#define STDMETHOD_(t, m) virtual t m
#define STDMETHODCALLTYPE
#define THIS_
#define THIS void
#define DECLARE_INTERFACE(i) struct i
#define DECLARE_INTERFACE_(i, b) struct i : public b
#define DEFINE_GUID(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = {a, b, c, {d0, d1, d2, d3, d4, d5, d6, d7}}
#define IsEqualGUID(a, b) (!memcmp(&(a), &(b), sizeof(GUID)))
#define MAKE_HRESULT(s, f, c) ((HRESULT)(((unsigned long)(s) << 31) | ((unsigned long)(f) << 16) | ((unsigned long)(c))))
struct IUnknown { virtual HRESULT QueryInterface(REFGUID, void**) { return E_FAIL; } virtual ULONG AddRef() { return 1; } virtual ULONG Release() { return 0; } };
#endif
