// DirectSound as the game uses it: every buffer a voice of the machine's sound card (dsound.cpp)
#ifndef AOE_DSOUND_H
#define AOE_DSOUND_H
#include "com.h"
#include "mmsystem.h"
#define DS_OK 0
#define DSERR_BUFFERLOST ((HRESULT)0x88780096)
#define DSSCL_NORMAL 1
#define DSSCL_PRIORITY 2
#define DSSCL_EXCLUSIVE 3
#define DSBCAPS_PRIMARYBUFFER 1
#define DSBCAPS_STATIC 2
#define DSBCAPS_CTRLFREQUENCY 0x20
#define DSBCAPS_CTRLPAN 0x40
#define DSBCAPS_CTRLVOLUME 0x80
#define DSBCAPS_CTRLDEFAULT 0xe0
#define DSBCAPS_GETCURRENTPOSITION2 0x10000
#define DSBPLAY_LOOPING 1
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_BUFFERLOST 2
#define DSBSTATUS_LOOPING 4
#define DSBVOLUME_MIN (-10000)
#define DSBVOLUME_MAX 0
#define DSBPAN_LEFT (-10000)
#define DSBPAN_RIGHT 10000
typedef struct _DSBUFFERDESC { DWORD dwSize, dwFlags, dwBufferBytes, dwReserved; LPWAVEFORMATEX lpwfxFormat; } DSBUFFERDESC, *LPDSBUFFERDESC;
typedef struct _DSCAPS { DWORD dwSize, dwFlags, rest[20]; } DSCAPS, *LPDSCAPS;
struct IDirectSoundBuffer : IUnknown {
    unsigned char* data; DWORD bytes, freq, status; int voice, vol, pan, primary; WAVEFORMATEX wf; unsigned phys; DWORD started;
    virtual ULONG Release();
    virtual HRESULT Lock(DWORD, DWORD, LPVOID*, LPDWORD, LPVOID*, LPDWORD, DWORD);
    virtual HRESULT Unlock(LPVOID, DWORD, LPVOID, DWORD);
    virtual HRESULT Play(DWORD, DWORD, DWORD);
    virtual HRESULT Stop();
    virtual HRESULT SetCurrentPosition(DWORD);
    virtual HRESULT GetCurrentPosition(LPDWORD, LPDWORD);
    virtual HRESULT GetStatus(LPDWORD);
    virtual HRESULT SetVolume(LONG);
    virtual HRESULT GetVolume(LONG*);
    virtual HRESULT SetPan(LONG);
    virtual HRESULT GetPan(LONG*);
    virtual HRESULT SetFrequency(DWORD);
    virtual HRESULT GetFrequency(LPDWORD);
    virtual HRESULT SetFormat(LPCWAVEFORMATEX);
    virtual HRESULT Restore() { return DS_OK; }
};
typedef IDirectSoundBuffer* LPDIRECTSOUNDBUFFER;
struct IDirectSound : IUnknown {
    virtual HRESULT SetCooperativeLevel(HWND, DWORD) { return DS_OK; }
    virtual HRESULT CreateSoundBuffer(LPDSBUFFERDESC, IDirectSoundBuffer**, IUnknown*);
    virtual HRESULT DuplicateSoundBuffer(IDirectSoundBuffer*, IDirectSoundBuffer**);
    virtual HRESULT GetCaps(LPDSCAPS);
};
typedef IDirectSound* LPDIRECTSOUND;
#define DirectSoundCreate aoe_DirectSoundCreate
HRESULT aoe_DirectSoundCreate(GUID*, IDirectSound**, IUnknown*);
#endif
