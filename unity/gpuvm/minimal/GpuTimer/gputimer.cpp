// A measure, not a part of a world (VRChat runs no native plugins): D3D11 timestamp queries put into Unity's render
// thread by GL.IssuePluginEvent (GvmProbe's camera hooks), so the card's time between two events is known. Event 0
// begins a frame (a disjoint query), event 1 ends it; any other event's time is summed under its number as the time
// from the event before it. Frames are read back four frames late, never waited for.
//   cl /LD /O2 /EHsc /I <Unity>/Data/PluginAPI gputimer.cpp d3d11.lib /Fe:GpuTimer.dll
#include <d3d11.h>
#include <string.h>
#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"

static ID3D11Device* device;
static ID3D11DeviceContext* ctx;
enum { FRAMES = 8, MARKS = 512, EVENTS = 64 };
struct Frame {
    ID3D11Query* disjoint;
    ID3D11Query* ts[MARKS];
    int ev[MARKS];
    int n;
    bool open, pending;
};
static Frame frames[FRAMES];
static int cur = -1;
static double sums[EVENTS];
static long long counts[EVENTS];
static long long framesRead, framesLost, eventsSeen, framesBegun;

static void Resolve(Frame& f) {
    if (!f.pending) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    if (ctx->GetData(f.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;   // (not yet: a later frame tries again)
    if (dj.Disjoint) { f.pending = false; framesLost++; return; }
    UINT64 prev = 0;
    for (int i = 0; i < f.n; i++) {
        UINT64 t;
        if (ctx->GetData(f.ts[i], &t, sizeof t, 0) != S_OK) { framesLost++; f.pending = false; return; }
        if (i > 0 && f.ev[i] < EVENTS) { sums[f.ev[i]] += (double)(t - prev) * 1000.0 / (double)dj.Frequency; counts[f.ev[i]]++; }
        prev = t;
    }
    framesRead++;
    f.pending = false;
}

static void Mark(Frame& f, int ev) {
    if (f.n >= MARKS) return;
    if (!f.ts[f.n]) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP, 0};
        device->CreateQuery(&d, &f.ts[f.n]);
    }
    ctx->End(f.ts[f.n]);
    f.ev[f.n++] = ev;
}

static void UNITY_INTERFACE_API OnEvent(int ev) {
    eventsSeen++;
    if (!device) return;
    if (ev == 0) {
        if (cur >= 0 && frames[cur].open) return;
        cur = (cur + 1) % FRAMES;
        Frame& f = frames[cur];
        if (f.pending) { framesLost++; f.pending = false; }   // (a frame never ready in eight)
        if (!f.disjoint) {
            D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            device->CreateQuery(&d, &f.disjoint);
        }
        ctx->Begin(f.disjoint);
        f.n = 0;
        f.open = true;
        framesBegun++;
        Mark(f, 0);
        for (int k = 1; k < FRAMES - 2; k++) Resolve(frames[(cur + k) % FRAMES]);   // (the frames three and more back, oldest first)
        return;
    }
    if (cur < 0 || !frames[cur].open) return;
    Frame& f = frames[cur];
    Mark(f, ev);
    if (ev == 1) {
        ctx->End(f.disjoint);
        f.open = false;
        f.pending = true;
    }
}

extern "C" {
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* u) {
    IUnityGraphicsD3D11* d3d = u->Get<IUnityGraphicsD3D11>();
    if (d3d) {
        device = d3d->GetDevice();
        if (device) device->GetImmediateContext(&ctx);
    }
}
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload() {}
// (a plugin the editor loads late gets no UnityPluginLoad: the device from any texture's native pointer)
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GpuTimer_Device(void* resource) {
    if (!device && resource) {
        ((ID3D11Resource*)resource)->GetDevice(&device);
        if (device) device->GetImmediateContext(&ctx);
    }
    return device != nullptr;
}
UNITY_INTERFACE_EXPORT UnityRenderingEvent UNITY_INTERFACE_API GpuTimer_Event() { return OnEvent; }
// the sums in ms and their counts for events 0..n-1; returns the frames read (lost ones in *lost)
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API GpuTimer_Read(double* ms, long long* n, int events, long long* lost) {
    for (int i = 0; i < events && i < EVENTS; i++) { ms[i] = sums[i]; n[i] = counts[i]; }
    *lost = framesLost | eventsSeen << 20 | framesBegun << 42;
    return (int)framesRead;
}
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API GpuTimer_Reset() {
    memset(sums, 0, sizeof sums);
    memset(counts, 0, sizeof counts);
    framesRead = framesLost = 0;
}
}
