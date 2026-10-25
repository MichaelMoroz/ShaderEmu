/*
 * q3_bot: Quake III's bots finding their way, botlib's routing. Copied from ioq3's botlib/be_aas_route.c:
 * AAS_UpdateAreaRoutingCache (the update list over reversed reachabilities, travel flags, clusters, the travel
 * times through an area), with the level's tables made here: 700 areas in one cluster on a grid with random
 * extra links (walk, jump, crouch, ladder, barrier jump: a reachability of a type each, about five an area),
 * area flags and contents. A unit: the routing caches to 3 goals made anew, then 24 bots each follow their
 * route to a goal area by area (AAS_AreaRouteToGoalArea's lookups: the cache's travel time and reachability).
 */
#include <string.h>
#include "wl.h"

#define AREAS 700
#define REACH_MAX 6
#define GOALS 3
#define BOTS 24
#define TFL_DEFAULT 0x1f
#define AREA_DISABLED 4

typedef struct { int areanum, traveltype; unsigned short traveltime; } aas_reachability_t;
typedef struct { int linknum, areanum; int next; } aas_reversedlink_t;    /* (next: an index, -1 at the end) */
typedef struct { int areaflags, contents, cluster, firstreachablearea, numreachableareas; } aas_areasettings_t;
typedef struct aas_routingupdate_s {
    int areanum, inlist;
    unsigned short tmptraveltime;
    const unsigned short *areatraveltimes;
    struct aas_routingupdate_s *next, *prev;
} aas_routingupdate_t;
typedef struct { int areanum, travelflags; unsigned short starttraveltime; unsigned short traveltimes[AREAS]; unsigned char reachabilities[AREAS]; } aas_routingcache_t;

static aas_reachability_t reachability[AREAS * REACH_MAX];
static aas_reversedlink_t revlinks[AREAS * REACH_MAX];
static int revfirst[AREAS];
static aas_areasettings_t areasettings[AREAS];
static unsigned short areatraveltimes[AREAS][REACH_MAX][REACH_MAX];
static int goals[GOALS], bot_area[BOTS];

static inline int AAS_TravelFlagForType_inline(int traveltype) { return 1 << (traveltype & 7); }
static inline int AAS_AreaContentsTravelFlags_inline(int areanum) { return areasettings[areanum].contents & 0x100 ? 0x100 : 0; }

struct scratch { aas_routingupdate_t areaupdate[AREAS]; aas_routingcache_t cache[GOALS]; };

static void AAS_UpdateAreaRoutingCache(aas_routingupdate_t *update, aas_routingcache_t *areacache)
{
    int i, nextareanum, linknum, badtravelflags = ~areacache->travelflags, clusterareanum = areacache->areanum;
    unsigned short t, startareatraveltimes[REACH_MAX] = {0};
    aas_routingupdate_t *updateliststart, *updatelistend, *curupdate, *nextupdate;
    curupdate = &update[clusterareanum];
    curupdate->areanum = areacache->areanum;
    curupdate->areatraveltimes = startareatraveltimes;
    curupdate->tmptraveltime = areacache->starttraveltime;
    areacache->traveltimes[clusterareanum] = areacache->starttraveltime;
    curupdate->next = curupdate->prev = 0;
    updateliststart = updatelistend = curupdate;
    while (updateliststart) {
        curupdate = updateliststart;
        if (curupdate->next) curupdate->next->prev = 0;
        else updatelistend = 0;
        updateliststart = curupdate->next;
        curupdate->inlist = 0;
        i = 0;
        for (int l = revfirst[curupdate->areanum]; l >= 0; l = revlinks[l].next, i++) {
            const aas_reachability_t *reach;
            linknum = revlinks[l].linknum;
            reach = &reachability[linknum];
            if (AAS_TravelFlagForType_inline(reach->traveltype) & badtravelflags) continue;
            if (areasettings[reach->areanum].areaflags & AREA_DISABLED) continue;
            if (AAS_AreaContentsTravelFlags_inline(reach->areanum) & badtravelflags) continue;
            nextareanum = revlinks[l].areanum;
            if (areasettings[nextareanum].cluster > 0 && areasettings[nextareanum].cluster != 1) continue;
            clusterareanum = nextareanum;
            /* (the times through an area: a row of REACH_MAX here, the real one as long as the area's reversed links) */
            t = (unsigned short)(curupdate->tmptraveltime + curupdate->areatraveltimes[i % REACH_MAX] + reach->traveltime);
            if (!areacache->traveltimes[clusterareanum] || areacache->traveltimes[clusterareanum] > t) {
                areacache->traveltimes[clusterareanum] = t;
                areacache->reachabilities[clusterareanum] = (unsigned char)(linknum - areasettings[nextareanum].firstreachablearea);
                nextupdate = &update[clusterareanum];
                nextupdate->areanum = nextareanum;
                nextupdate->tmptraveltime = t;
                nextupdate->areatraveltimes = areatraveltimes[nextareanum][linknum - areasettings[nextareanum].firstreachablearea];
                if (!nextupdate->inlist) {
                    nextupdate->next = 0;
                    nextupdate->prev = updatelistend;
                    if (updatelistend) updatelistend->next = nextupdate;
                    else updateliststart = nextupdate;
                    updatelistend = nextupdate;
                    nextupdate->inlist = 1;
                }
            }
        }
    }
}

static void setup(void)
{
    uint32_t seed = 0xb07;
    int side = 28, links = 0;
    for (int a = 0; a < AREAS; a++) {
        aas_areasettings_t *s = &areasettings[a];
        s->cluster = 1;
        s->areaflags = wl_rand(&seed) % 60 == 0 ? AREA_DISABLED : 0;
        s->contents = wl_rand(&seed) % 25 == 0 ? 0x100 : 0;   /* (water, say: not for the default flags) */
        s->firstreachablearea = links;
        int n = 3 + (int)(wl_rand(&seed) % (REACH_MAX - 2));
        for (int k = 0; k < n; k++) {
            int to;
            switch (k) {
            case 0: to = a + 1; break;
            case 1: to = a - 1; break;
            case 2: to = a + side; break;
            case 3: to = a - side; break;
            default: to = (int)(wl_rand(&seed) % AREAS);
            }
            if (to < 0 || to >= AREAS || to == a) to = (a + 7) % AREAS;
            reachability[links] = (aas_reachability_t){to, k < 4 ? 2 : 2 + (int)(wl_rand(&seed) % 5), (unsigned short)(20 + wl_rand(&seed) % 200)};
            links++;
        }
        s->numreachableareas = n;
        for (int i = 0; i < REACH_MAX; i++)
            for (int j = 0; j < REACH_MAX; j++) areatraveltimes[a][i][j] = (unsigned short)(10 + wl_rand(&seed) % 90);
    }
    for (int a = 0; a < AREAS; a++) revfirst[a] = -1;
    for (int a = 0; a < AREAS; a++)   /* reversed: for the area a link leads to, the link and where it comes from */
        for (int k = 0; k < areasettings[a].numreachableareas; k++) {
            int linknum = areasettings[a].firstreachablearea + k, to = reachability[linknum].areanum;
            revlinks[linknum] = (aas_reversedlink_t){linknum, a, revfirst[to]};
            revfirst[to] = linknum;
        }
    for (int g = 0; g < GOALS; g++) goals[g] = (int)(wl_rand(&seed) % AREAS);
    for (int b = 0; b < BOTS; b++) bot_area[b] = (int)(wl_rand(&seed) % AREAS);
}

static void *inst(void) { return wl_alloc(sizeof(struct scratch)); }

static uint32_t run(void *ctx)
{
    struct scratch *s = ctx;
    uint32_t sum = 0;
    for (int g = 0; g < GOALS; g++) {
        aas_routingcache_t *c = &s->cache[g];
        memset(c, 0, sizeof *c);
        memset(s->areaupdate, 0, sizeof s->areaupdate);
        c->areanum = goals[g], c->travelflags = TFL_DEFAULT | 0x7c, c->starttraveltime = 1;
        AAS_UpdateAreaRoutingCache(s->areaupdate, c);
    }
    for (int b = 0; b < BOTS; b++) {   /* AAS_AreaRouteToGoalArea, step by step to the goal */
        const aas_routingcache_t *c = &s->cache[b % GOALS];
        int area = bot_area[b], steps = 0;
        while (area != c->areanum && c->traveltimes[area] && steps < 200) {
            int reach = areasettings[area].firstreachablearea + c->reachabilities[area];
            sum += c->traveltimes[area];
            area = reachability[reach].areanum;
            steps++;
        }
        sum = sum * 31 + (uint32_t)steps;
    }
    return sum;
}

const wl_kernel wl_q3_bot = {"q3_bot", "Quake III bots: AAS_UpdateAreaRoutingCache and route following", setup, inst, run};
