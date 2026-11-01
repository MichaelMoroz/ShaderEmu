#ifndef AOE_MMSYSTEM_H
#define AOE_MMSYSTEM_H
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX, *LPWAVEFORMATEX, *PWAVEFORMATEX;
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;
typedef struct waveformat_tag { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign; } WAVEFORMAT, *LPWAVEFORMAT;
typedef struct pcmwaveformat_tag { WAVEFORMAT wf; WORD wBitsPerSample; } PCMWAVEFORMAT;
#pragma pack(pop)
#define WAVE_FORMAT_PCM 1
typedef DWORD MCIERROR; typedef UINT MCIDEVICEID, MMRESULT; typedef DWORD FOURCC;
typedef struct _MMCKINFO { FOURCC ckid; DWORD cksize; FOURCC fccType; DWORD dwDataOffset, dwFlags; } MMCKINFO, *LPMMCKINFO;
#define TIMERR_NOERROR 0
#define MAKEFOURCC(a,b,c,d) ((DWORD)(BYTE)(a)|((DWORD)(BYTE)(b)<<8)|((DWORD)(BYTE)(c)<<16)|((DWORD)(BYTE)(d)<<24))
#define mmioFOURCC MAKEFOURCC
#define MCI_OPEN 0x803
#define MCI_CLOSE 0x804
#define MCI_PLAY 0x806
#define MCI_SEEK 0x807
#define MCI_STOP 0x808
#define MCI_PAUSE 0x809
#define MCI_SET 0x80d
#define MCI_STATUS 0x814
#define MCI_RESUME 0x855
#define MCI_NOTIFY 1
#define MCI_WAIT 2
#define MCI_FROM 4
#define MCI_TO 8
#define MCI_TRACK 0x10
#define MCI_OPEN_SHAREABLE 0x100
#define MCI_OPEN_ELEMENT 0x200
#define MCI_OPEN_TYPE_ID 0x1000
#define MCI_OPEN_TYPE 0x2000
#define MCI_SET_DOOR_OPEN 0x100
#define MCI_SET_TIME_FORMAT 0x400
#define MCI_STATUS_ITEM 0x100
#define MCI_STATUS_LENGTH 1
#define MCI_STATUS_POSITION 2
#define MCI_STATUS_NUMBER_OF_TRACKS 3
#define MCI_STATUS_MODE 4
#define MCI_STATUS_MEDIA_PRESENT 5
#define MCI_STATUS_CURRENT_TRACK 8
#define MCI_MODE_PLAY 526
#define MCI_FORMAT_TMSF 10
#define MCI_DEVTYPE_CD_AUDIO 516
#define MCI_DEVTYPE_SEQUENCER 523
#define MCI_SEQ_MAPPER 0xffff
#define MCI_MCIAVI_PLAY_WINDOW 0x1000000
#define MM_MCINOTIFY 0x3B9
#define MCI_NOTIFY_SUCCESSFUL 1
#define MCI_NOTIFY_ABORTED 4
#define MCI_MAKE_TMSF(t,m,s,f) ((DWORD)(((BYTE)(t)|((WORD)(m)<<8))|(((DWORD)(BYTE)(s)|((WORD)(f)<<8))<<16)))
#define MCI_TMSF_TRACK(t) ((BYTE)(t))
typedef struct { DWORD dwCallback; MCIDEVICEID wDeviceID; LPCSTR lpstrDeviceType, lpstrElementName, lpstrAlias; } MCI_OPEN_PARMS, MCI_OPEN_PARMSA, *LPMCI_OPEN_PARMS;
typedef struct { DWORD dwCallback, dwFrom, dwTo; } MCI_PLAY_PARMS, *LPMCI_PLAY_PARMS;
typedef struct MCI_STATUS_PARMS_T { DWORD dwCallback, dwReturn, dwItem, dwTrack; } MCI_STATUS_PARMS, *LPMCI_STATUS_PARMS;
typedef struct { DWORD dwCallback, dwTo; } MCI_SEEK_PARMS, *LPMCI_SEEK_PARMS;
typedef struct { DWORD dwCallback, dwTimeFormat, dwAudio; } MCI_SET_PARMS, *LPMCI_SET_PARMS;
typedef struct { DWORD dwCallback; } MCI_GENERIC_PARMS;
#ifdef __cplusplus
extern "C" {
#endif
DWORD mciSendCommandA(UINT, UINT, DWORD, DWORD);
BOOL mciGetErrorStringA(DWORD, LPSTR, UINT);
#ifdef __cplusplus
}
#endif
#define mciSendCommand mciSendCommandA
#define mciGetErrorString mciGetErrorStringA
#endif
