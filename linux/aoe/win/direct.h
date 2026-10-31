#include <unistd.h>
#define _getcwd getcwd
extern "C" int aoe_chdir(const char*);
#define _chdir aoe_chdir
extern "C" int _chdrive(int);
extern "C" int _getdrive(void);
#define _mkdir(n) mkdir((n), 0777)
