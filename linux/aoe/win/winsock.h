#ifndef AOE_WINSOCK_H
#define AOE_WINSOCK_H
// the game's addresses are for network games, and there are none
typedef struct WSAData { WORD wVersion, wHighVersion; char szDescription[257], szSystemStatus[129]; unsigned short iMaxSockets, iMaxUdpDg; char* lpVendorInfo; } WSADATA, *LPWSADATA;
struct hostent { char* h_name; char** h_aliases; short h_addrtype, h_length; char** h_addr_list; };
static inline int WSAStartup(WORD, LPWSADATA) { return 1; }
static inline int WSACleanup(void) { return 0; }
static inline struct hostent* gethostbyname_win(const char*) { return 0; }
#define gethostbyname gethostbyname_win
static inline int gethostname_win(char* n, int s) { if (s) n[0] = 0; return 0; }
#define gethostname gethostname_win
struct in_addr_win { unsigned long s_addr; };
#endif
