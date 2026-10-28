/* Shared runtime overlay derived from LAS Recomp Android at
 * cca791a3e7218e1dac95e7d4a11b8f97dba3ea51, Copyright (c) 2026 Peiikai.
 * Android uint32/read-only snapshot adaptation also derives from Let's Go Recomp
 * 96894fd8e9ea20c1b38cdea53105ec2a335df620, Copyright (c) 2026 raulzito12345rrs.
 * MIT notices: native/runtime/LICENSE-LASRecomp and LICENSE-LetsGo. */
// Cosmetics: Random enemy colours (Cosmetics tab "Enemy colors": Off / Random per seed / Random every load), live. Each
// enemy species gets its own hue shift (its colour maps, actor/Enemy<species>.bfres, are recognised: recolour.c), the
// same for every enemy of that species; greys and whites (bones, eyes, teeth) stay. Per seed: the shifts follow the
// randomizer seed's name (vanilla: one fixed set). Every load: new shifts whenever a new level loads (a house, a cave,
// a dungeon, back to the overworld).
//
// Never recoloured - species whose colour tells strength, type or what to do (their variants differ by colour only):
//   Octorok        red / blue Octorok-style pairs (colour set per actor, same map)
//   Hinox          its variants are colour swaps of one body (brown / red / blue)
//   ZolGreen, ZolRed, BossBlob, DeguZol   Zols and Gels: green / red Zols behave differently; the Slime Eye's and
//                  Slime Eel-style bosses use the same colour coding
//   StalfosGreen, StalfosOrange          green and orange Stalfos behave differently
//   BomberGreen, BomberRed               green / red Bombites: different explosions
//   KarakoroGreen  Color Dungeon soldiers: thrown into the pit of their colour
//   StretchyGhostBlue                    blue / green / red variants of one body
//   Vegas          the cards' suits are the puzzle (match three)
//   DonPawn        its shell colour shows how hurt it is (blue .. red)
//   ArmosDark, ArmosLight                the colour change is the statue coming alive
//   Hidy           its variants differ only by colour (and it hides as a bush / rock / skull: those stay as they are)
#include <string.h>
#include <time.h>

#include "../game_addresses.h"
#include "../common_session.h"
#include "../hle.h"
#include "../settings.h"
#include "recolour.h"

#define E_BASE 64

#define SPECIES(X)                                                                                                      \
    X(Albatoss) X(AnaMonea) X(Angler) X(Ballerine) X(BombKnuckle) X(BonePutter) X(Bossblin) X(Bubble) X(BuzzBlob)      \
    X(Creepee) X(Crowly) X(DeguArmos) X(DeguFlame) X(DeguGhini) X(DeguTail) X(Dodongo) X(FlyingOctorok) X(Gesso)       \
    X(Ghini) X(GiantBubble) X(Gibdos) X(Gohma) X(Heiho) X(Hiploop) X(HiploopHover) X(Hooker) X(HookerFake)            \
    X(HookerTail) X(Hover) X(Jacky) X(Keese) X(Kirby) X(Kuribo) X(Kyorobo) X(Kyune) X(Lanmola) X(Leever) X(LikeLike)  \
    X(MasterStalfon) X(MatFace) X(MightPunch) X(Mimic) X(Minigler) X(MoblinSpear) X(MoblinSword) X(Monea)             \
    X(MoriblinSpear) X(MoriblinSword) X(Nomos) X(Pakkun) X(Pawn) X(Peahat) X(Peese) X(Piccolo) X(PiccoloMaster)        \
    X(Pincer) X(Piranha) X(Pokey) X(PolsVoice) X(PotDemonKing) X(PotDemonPot) X(PukuPuku) X(Rola) X(Rope)             \
    X(SeaUrchin) X(ShadowDethI) X(SoldierIronBall) X(SoldierSpear) X(SoldierSword) X(Spark) X(StalKnightSpear)         \
    X(StalKnightSword) X(StoneHinox) X(Tail) X(Tektight) X(Teresa) X(Togezo) X(Totakeke) X(TurtleRock) X(Vire)          \
    X(VireChild) X(Wizzrobe) X(Zombie) X(Zora) X(ZoroZoro)

static int classify(int source, const char *t) {  // every colour map of the species ("..._alb", "..._alb.<n>")
    return strstr(t, "_alb") ? E_BASE + source : 0;
}

#define SOURCE(n) {"region_common/actor/Enemy" #n ".bfres", classify},
static const RecolourSource k_sources[] = {SPECIES(SOURCE)};
enum { N_SPECIES = (int)(sizeof k_sources / sizeof *k_sources) };

static uint32_t mix32(uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

// Every load: a new set whenever the level's name (the player status' first field) changes
static uint32_t load_seed(void) {
    if (!shared_title_las()) return 0;
    static uint32_t seed;
    static uint64_t frame = ~0ull;
    static char level[64];
    extern uint64_t renderer_present_count(void);
    uint64_t f = renderer_present_count();
    if (f != frame) {  // (once a frame)
        frame = f;
        uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
        const char *name = ps ? (const char *)(uintptr_t)ps : "";
        if (!seed || strncmp(name, level, sizeof level - 1)) {
            strlcpy(level, name, sizeof level);
            seed = mix32((uint32_t)time(NULL) ^ (uint32_t)f * 0x9E3779B1u ^ (seed + 1));
        }
    }
    return seed;
}

static uint32_t seed_seed(void) {  // Per seed: the randomizer seed's name (vanilla: "")
    uint32_t h = 2166136261u;
    for (const char *p = g_settings.rando_seed; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static bool tint(int cls, float t[8]) {
    if (!shared_title_las()) return false;
    extern bool cosmetics_seed_on(void);  // (Off: a seed with its own cosmetics has them per seed)
    int mode = g_settings.enemy_colours ? g_settings.enemy_colours : cosmetics_seed_on() ? 1 : 0;
    if (mode <= 0 || mode >= N_ENEMY_COLOUR_MODES || cls < E_BASE || cls >= E_BASE + N_SPECIES) return false;
    uint32_t h = mix32((mode == 2 ? load_seed() : seed_seed()) ^ (uint32_t)(cls - E_BASE) * 0x27D4EB2Fu);
    float shift = 0.12f + (h >> 8) / 16777216.0f * 0.76f;  // (12-88% of the wheel: always clearly another colour)
    recolour_pack(t, shift, 1, 1, 0, 0.5f, 0.15f, 1);
    return true;
}

const RecolourOption g_recolour_enemies = {"enemy colours", k_sources, N_SPECIES, E_BASE, E_BASE + N_SPECIES - 1, tint};
