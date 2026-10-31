// Micropolis's engine behind mp.h: the engine's callbacks become a queue the front end reads.
#include "micropolis.h"
#include "mp.h"
#include <new>
#include <stdlib.h>
#include <string.h>

extern "C" int mp_fixed_seed;	// random.cpp (our patch): a seed in place of the clock's, when not 0
int mp_fixed_seed;

static Micropolis* city;
static mp_said said[64];
static int said_first, said_count;

static mp_said* say(int kind)
{
    mp_said* s;

    if (said_count == 64) {
        said_first = (said_first + 1) & 63;
        said_count--;
    }
    s = &said[(said_first + said_count++) & 63];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    return s;
}

struct Front : Callback {
    void autoGoto(Micropolis*, emscripten::val, int, int, std::string) override {}
    void didGenerateMap(Micropolis*, emscripten::val, int) override {}
    void didLoadCity(Micropolis*, emscripten::val, std::string) override {}
    void didLoadScenario(Micropolis*, emscripten::val, std::string, std::string) override {}
    void didLoseGame(Micropolis*, emscripten::val) override { say(MP_SAID_LOST); }
    void didSaveCity(Micropolis*, emscripten::val, std::string) override {}
    void didTool(Micropolis*, emscripten::val, std::string, int, int) override {}
    void didWinGame(Micropolis*, emscripten::val) override { say(MP_SAID_WON); }
    void didntLoadCity(Micropolis*, emscripten::val, std::string) override {}
    void didntSaveCity(Micropolis*, emscripten::val, std::string) override {}
    void makeSound(Micropolis*, emscripten::val, std::string, std::string sound, int x, int y) override
    {
        mp_said* s = say(MP_SAID_SOUND);

        strncpy(s->sound, sound.c_str(), sizeof s->sound - 1);
        s->x = x, s->y = y;
    }
    void newGame(Micropolis*, emscripten::val) override {}
    void saveCityAs(Micropolis*, emscripten::val, std::string) override {}
    void sendMessage(Micropolis*, emscripten::val, int index, int x, int y, bool, bool) override
    {
        mp_said* s = say(MP_SAID_MESSAGE);

        s->number = index, s->x = x, s->y = y;
    }
    void showBudgetAndWait(Micropolis*, emscripten::val) override { say(MP_SAID_BUDGET); }
    void showZoneStatus(Micropolis*, emscripten::val, int category, int density, int value, int crime, int pollution,
                        int growth, int x, int y) override
    {
        mp_said* s = say(MP_SAID_QUERY);

        s->query[0] = category, s->query[1] = density, s->query[2] = value;
        s->query[3] = crime, s->query[4] = pollution, s->query[5] = growth;
        s->x = x, s->y = y;
    }
    void simulateRobots(Micropolis*, emscripten::val) override {}
    void simulateChurch(Micropolis*, emscripten::val, int, int, int) override {}
    void startEarthquake(Micropolis*, emscripten::val, int) override {}
    void startGame(Micropolis*, emscripten::val) override {}
    void startScenario(Micropolis*, emscripten::val, int) override {}
    void updateBudget(Micropolis*, emscripten::val) override {}
    void updateCityName(Micropolis*, emscripten::val, std::string) override {}
    void updateDate(Micropolis*, emscripten::val, int, int) override {}
    void updateDemand(Micropolis*, emscripten::val, float, float, float) override {}
    void updateEvaluation(Micropolis*, emscripten::val) override {}
    void updateFunds(Micropolis*, emscripten::val, int) override {}
    void updateGameLevel(Micropolis*, emscripten::val, int) override {}
    void updateHistory(Micropolis*, emscripten::val) override {}
    void updateMap(Micropolis*, emscripten::val) override {}
    void updateOptions(Micropolis*, emscripten::val) override {}
    void updatePasses(Micropolis*, emscripten::val, int) override {}
    void updatePaused(Micropolis*, emscripten::val, bool) override {}
    void updateSpeed(Micropolis*, emscripten::val, int) override {}
    void updateTaxRate(Micropolis*, emscripten::val, int) override {}
};

extern "C" {

int mp_init(void)
{
    city = new Micropolis;
    if (!city)
        return -1;
    city->setCallback(new Front, emscripten::val());
    city->init();
    city->setAutoBudget(true);
    city->setSpeed(3);
    return 0;
}

int mp_load(const char* path)
{
    return city->loadCity(path) ? 0 : -1;
}

void mp_scenario(int n)
{
    city->loadScenario((Scenario)n);
}

void mp_generate(int seed)
{
    city->generateSomeCity(seed);
    city->setGameLevelFunds(LEVEL_EASY); // else the last city's funds go on
    city->setCityName("Newtown");
}

int mp_save(const char* path)
{
    return city->saveFile(path) ? 0 : -1;
}

void mp_seed(int seed)
{
    mp_fixed_seed = seed;
    city->seedRandom(seed);
}

void mp_tick(void)
{
    city->simTick();
}

void mp_view(int x0, int y0, int x1, int y1, unsigned short* ring, int across, int animate)
{
    unsigned short* map = (unsigned short*)city->getMapBuffer();

    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= MP_W ? MP_W - 1 : x1, y1 = y1 >= MP_H ? MP_H - 1 : y1;
    for (int x = x0; x <= x1; x++) {
        unsigned short* cell = map + x * MP_H + y0;
        unsigned short* to = ring + (x & (across - 1));

        for (int y = y0; y <= y1; y++, cell++) {
            unsigned v = *cell;

            if (animate && (v & ANIMBIT))
                *cell = (unsigned short)(v = (v & ALLBITS) | city->getNextAnimatedTile(v & LOMASK));
            to[(y & (across - 1)) * across] = (unsigned short)(v & LOMASK);
        }
    }
}

const unsigned short* mp_map(void)
{
    return (const unsigned short*)city->getMapBuffer();
}

int mp_sprites(struct mp_sprite* to, int most)
{
    int n = 0;

    for (SimSprite* s = city->spriteList; s && n < most; s = s->next)
        if (s->frame > 0 && s->type >= 1 && s->type <= 8) {
            to[n].type = s->type, to[n].frame = s->frame;
            to[n].x = s->x + s->xOffset, to[n].y = s->y + s->yOffset;
            n++;
        }
    return n;
}

int mp_tool(int tool, int x, int y)
{
    return city->doTool((EditingTool)tool, (short)x, (short)y);
}

int mp_tool_size(int tool)
{
    static const short size[MP_TOOLS] = {3, 3, 3, 3, 3, 1, 1, 1, 1, 1, 4, 1, 4, 4, 4, 6};
    return tool >= 0 && tool < MP_TOOLS ? size[tool] : 1;
}

int mp_tool_cost(int tool)
{
    static const short cost[MP_TOOLS] = {100, 100, 100, 500, 500, 0, 5, 1, 20, 10, 5000, 10, 3000, 3000, 5000, 10000};
    return tool >= 0 && tool < MP_TOOLS ? cost[tool] : 0;
}

void mp_status(struct mp_status* to)
{
    to->funds = (int)city->totalFunds;
    to->year = (int)(city->cityTime / 48) + city->startingYear;
    to->month = (int)(city->cityTime % 48) >> 2;
    to->population = (int)city->cityPop;
    to->tax = city->cityTax;
    to->score = city->cityScore;
    to->residential = city->resValve, to->commercial = city->comValve, to->industrial = city->indValve;
    to->speed = city->simSpeedMeta;
    to->paused = city->simPaused;
    to->auto_budget = city->autoBudget;
    to->disasters = city->enableDisasters;
    to->scenario = city->scenario;
    strncpy(to->name, city->cityName.c_str(), sizeof to->name - 1);
    to->name[sizeof to->name - 1] = 0;
}

void mp_set_speed(int speed)
{
    city->setSpeed((short)speed);
}

void mp_pause(int paused)
{
    if (paused && !city->simPaused)
        city->pause();
    else if (!paused && city->simPaused)
        city->resume();
}

void mp_set_tax(int percent)
{
    city->setCityTax((short)percent);
}

void mp_set_auto_budget(int on)
{
    city->setAutoBudget(on != 0);
}

void mp_set_disasters(int on)
{
    city->setEnableDisasters(on != 0);
}

void mp_disaster(int which)
{
    switch (which) {
    case MP_FIRE_DISASTER: city->makeFire(); break;
    case MP_FLOOD: city->makeFlood(); break;
    case MP_TORNADO: city->makeTornado(); break;
    case MP_EARTHQUAKE: city->makeEarthquake(); break;
    case MP_MONSTER: city->makeMonster(); break;
    case MP_MELTDOWN: city->makeMeltdown(); break;
    }
}

int mp_next_said(struct mp_said* to)
{
    if (!said_count)
        return 0;
    *to = said[said_first];
    said_first = (said_first + 1) & 63;
    said_count--;
    return to->kind;
}

static int level_of(int value)	// the engine's getCI: none, low, medium, high, very high
{
    return value < 50 ? 0 : value < 100 ? 1 : value < 150 ? 2 : value < 200 ? 3 : 4;
}

// Whether a layer of zones shows a tile: the tile filters of map.cpp's drawRes and the others.
static bool shown_in(int show, int tile)
{
    switch (show) {
    case MP_SHOW_RESIDENTIAL: return tile < COMBASE;
    case MP_SHOW_COMMERCIAL: return !(tile > COMLAST || (tile >= LVRAIL6 && tile < COMBASE));
    case MP_SHOW_INDUSTRIAL:
        return !((tile >= RESBASE && tile < INDBASE) || (tile >= PORTBASE && tile < SMOKEBASE) ||
                 (tile >= TINYEXP && tile <= TINYEXPLAST) || tile >= FOOTBALLGAME1);
    case MP_SHOW_TRANSPORT: return !(tile >= RESBASE || (tile >= BRWXXX7 && tile <= LVPOWER10) || tile == UNUSED_TRASH6);
    }
    return true;
}

void mp_overview(int show, unsigned short* tiles, unsigned char* levels)
{
    const unsigned short* map = (const unsigned short*)city->getMapBuffer();

    for (int y = 0; y < MP_H; y++)
        for (int x = 0; x < MP_W; x++) {
            unsigned v = map[x * MP_H + y];
            int tile = v & LOMASK, level = 0;

            tile = tile >= TILE_COUNT ? tile - TILE_COUNT : tile;
            if (!shown_in(show, tile))
                tile = DIRT;
            switch (show) {
            case MP_SHOW_POWER:
                if (tile > LASTFIRE)
                    level = (v & ZONEBIT) ? ((v & PWRBIT) ? 2 : 1) : (v & CONDBIT) ? 3 : 0;
                if (tile > LASTFIRE && !level)
                    tile = DIRT;
                break;
            case MP_SHOW_DENSITY: level = level_of(city->populationDensityMap.worldGet(x, y)); break;
            case MP_SHOW_TRAFFIC: level = level_of(city->trafficDensityMap.worldGet(x, y)); break;
            case MP_SHOW_POLLUTION: level = level_of(city->pollutionDensityMap.worldGet(x, y)); break;
            case MP_SHOW_CRIME: level = level_of(city->crimeRateMap.worldGet(x, y)); break;
            case MP_SHOW_VALUE: level = level_of(city->landValueMap.worldGet(x, y)); break;
            case MP_SHOW_FIRE: level = level_of(city->fireStationEffectMap.worldGet(x, y)); break;
            case MP_SHOW_POLICE: level = level_of(city->policeStationEffectMap.worldGet(x, y)); break;
            case MP_SHOW_GROWTH: {
                int z = city->rateOfGrowthMap.worldGet(x, y);
                level = z > 100 ? 4 : z > 20 ? 3 : z < -100 ? 1 : z < -20 ? 2 : 0;
                break;
            }
            }
            tiles[y * MP_W + x] = (unsigned short)tile;
            levels[y * MP_W + x] = (unsigned char)level;
        }
    // a zone's power is its centre's: the whole zone shows it (tool.cpp's checkSize, and the airport)
    if (show == MP_SHOW_POWER)
        for (int y = 0; y < MP_H; y++)
            for (int x = 0; x < MP_W; x++) {
                unsigned v = map[x * MP_H + y];
                int tile = v & LOMASK, size, level = levels[y * MP_W + x];

                if (!(v & ZONEBIT) || level > 2)
                    continue;
                size = (tile >= RESBASE - 1 && tile <= PORTBASE - 1) || (tile >= LASTPOWERPLANT + 1 && tile <= POLICESTATION + 4) ||
                       (tile >= CHURCH1BASE && tile <= CHURCH7LAST) ? 3 : tile >= AIRPORTBASE && tile < COALBASE ? 6 : 4;
                for (int dy = -1; dy < size - 1; dy++)
                    for (int dx = -1; dx < size - 1; dx++)
                        if (x + dx >= 0 && x + dx < MP_W && y + dy >= 0 && y + dy < MP_H)
                            levels[(y + dy) * MP_W + x + dx] = (unsigned char)level;
            }
}

void mp_budget(struct mp_budget* to)
{
    Quad wanted[3] = {city->roadFund, city->fireFund, city->policeFund};
    float percent[3] = {city->roadPercent, city->firePercent, city->policePercent};
    Quad spent[3] = {city->roadSpend, city->fireSpend, city->policeSpend};

    to->tax_rate = city->cityTax;
    to->taxes = (int)city->taxFund;
    to->cash_flow = to->taxes;
    for (int k = 0; k < 3; k++) {
        to->wanted[k] = (int)wanted[k];
        to->percent[k] = (int)(percent[k] * 100.0f + 0.5f);
        to->spent[k] = (int)spent[k];
        to->cash_flow -= (int)(wanted[k] * percent[k]);
    }
    to->funds = (int)city->totalFunds;
}

void mp_set_budget(const int percent[3])
{
    Quad before = city->roadSpend + city->fireSpend + city->policeSpend;

    city->roadPercent = percent[0] / 100.0f;
    city->firePercent = percent[1] / 100.0f;
    city->policePercent = percent[2] / 100.0f;
    city->roadSpend = (Quad)(city->roadFund * city->roadPercent);
    city->fireSpend = (Quad)(city->fireFund * city->firePercent);
    city->policeSpend = (Quad)(city->policeFund * city->policePercent);
    {
        /* no more than there is: what would pass the funds comes off the police, the fire, then the roads */
        Quad over = city->roadSpend + city->fireSpend + city->policeSpend - before - city->totalFunds;
        Quad *spend[3] = { &city->policeSpend, &city->fireSpend, &city->roadSpend };
        float *share[3] = { &city->policePercent, &city->firePercent, &city->roadPercent };
        Quad fund[3] = { city->policeFund, city->fireFund, city->roadFund };

        for (int i = 0; i < 3 && over > 0; i++) {
            Quad off = over < *spend[i] ? over : *spend[i];

            *spend[i] -= off;
            over -= off;
            *share[i] = fund[i] ? (float)*spend[i] / fund[i] : 0.0f;
        }
    }
    city->spend(city->roadSpend + city->fireSpend + city->policeSpend - before);
    city->updateFundEffects();
}

void mp_evaluation(struct mp_evaluation* to)
{
    to->yes = city->cityYes;
    to->score = city->cityScore;
    to->score_delta = city->cityScoreDelta;
    to->population = (int)city->cityPop;
    to->population_delta = (int)city->cityPopDelta;
    to->assessed = (int)city->cityAssessedValue;
    to->city_class = city->cityClass;
    to->level = city->gameLevel;
    for (int i = 0; i < 4; i++) {
        to->problems[i] = city->getProblemNumber(i);
        to->votes[i] = city->getProblemVotes(i);
    }
}

void mp_history(int which, int years120, short* to)
{
    for (int i = 0; i < 120; i++)
        to[i] = city->getHistory(which, years120 ? HISTORY_SCALE_LONG : HISTORY_SCALE_SHORT, i);
}

unsigned mp_sum(void)
{
    unsigned sum = 0;

    for (int i = 0; i < MP_W * MP_H; i++)
        sum = sum * 31 + mp_map()[i];
    return sum ^ (unsigned)city->totalFunds * 2654435761u ^ (unsigned)city->cityPop;
}

int mp_sum_test(const char* spec)
{
    char what[256];
    int ticks = 0, seed = 1, scenario;
    struct mp_status s;

    if (sscanf(spec, "%255[^,],%d,%d", what, &ticks, &seed) < 2 || mp_init() < 0)
        return -1;
    mp_seed(seed);
    scenario = atoi(what);
    if (scenario > 0)
        mp_scenario(scenario);
    else if (mp_load(what) < 0) {
        printf("micropolis: %s does not load\n", what);
        return -1;
    }
    mp_seed(seed);
    for (int i = 0; i < ticks; i++) {
        mp_tick();
        city->animateTiles();
    }
    mp_status(&s);
    printf("micropolis: %s, %d ticks: %d/%d, population %d, funds %d, sum %08x\n", what, ticks, s.month + 1, s.year,
           s.population, s.funds, mp_sum());
    return 0;
}
}
