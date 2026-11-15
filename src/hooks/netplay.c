/* netplay.c -- lockstep netplay for Def Jam: Fight for NY (Recompiled).
 *
 * How the title takes its input (docs/research/simulation-tick.md, verified in the lifted code):
 * a 60 Hz worker (sub_000D7700 -> sub_000D7AE0) snapshots the four controller ports into a
 * ring of ten 0x260-byte records in the frame queue object (0x3A017C points at it). A record
 * is four status words (1 = no controller, 2 = controller present) and four 0x94-byte reports.
 * The main loop's step gate, sub_000D75E0, copies the oldest record into the staging buffer
 * at queue+0x17D0 and returns 1 when one simulation update is owed; that update then reads
 * its controllers from the staging buffer only (sub_000DD930 and the controller manager).
 *
 * So one simulation update == one staged record, and the staged record is the update's whole
 * input. Netplay replaces it: after the gate says "step", this machine's own controller (the
 * lowest present port of the record it just staged) is sent as its input for update N + delay,
 * and the staging buffer is overwritten with every player's input for update N, player P on
 * port P. Every machine therefore runs the same updates with the same input, menus included.
 *
 * What else would make two machines part, and what this file does about it:
 *  - Seeds the title takes from the time-stamp counter (the match generator and its increment,
 *    the four AI generators, the match logic's two generators, the general-purpose one): all
 *    replaced by values derived from the session seed (test_telemetry.c's seed hooks ask here).
 *  - Spectators animate with the host's frame timing and fighters react to them (deck-
 *    determinism-98.md): the fighters are shown no spectators, and the general-purpose
 *    generator restarts from the step number at every step, as RECOMP_TEST_ISOLATE_CROWD does.
 *  - Anything not found yet: every 60 updates each machine hashes the match state and the host
 *    compares; a difference is reported (log and title bar) as a desync at that update.
 *
 * Configuration, from the environment (the netplay .bat files set it):
 *   RECOMP_NETPLAY=host | join          off when unset
 *   RECOMP_NETPLAY_ADDR=<host address>  join: the host's address (its Radmin VPN IP works)
 *   RECOMP_NETPLAY_PORT=<udp port>      27015 by default
 *   RECOMP_NETPLAY_PLAYERS=<2..4>       host: how many players to wait for
 *   RECOMP_NETPLAY_DELAY=<frames>       0/unset: chosen from the measured ping
 *   RECOMP_NETPLAY_NAME=<name>
 *   RECOMP_NETPLAY_LOCAL_PORT=<0..3>    which local controller port is "me" (lowest present one by default)
 */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "netplay.h"
#include "netplay_net.h"
#include "netplay_lobby.h"

#define FRAME_QUEUE_PTR   0x003A017Cu   /* the frame queue object */
#define STAGING           0x17D0u       /* staged record inside it */
#define REPORT_BYTES      0x94u
#define PORTS             4
#define BLOB_BYTES        (4u + REPORT_BYTES)   /* status word + report */

#define ACTORS            0x003B92E0u
#define ACTOR_STRIDE      0x12E8u
#define F_SLOT            0x28u
#define F_CHARACTER       0x2Cu
#define F_STATE           0x30u
#define F_POS_X           0x44u
#define F_FACING          0x54u
#define F_HEALTH          0x350u
#define CLOGIC_STATE      (0x00349760u + 0x2D820u)
#define G_PHASE           0x003B8F30u
#define G_RNG_STATE       0x003C1404u
#define G_RNG_INC         0x003C1400u
#define G_RNG_UTIL        0x0039C160u
#define AI_GEN_STATE      0x003BFCF0u   /* + slot * 0x88 */
#define SPECTATOR_COUNT   0x003494E0u

static int s_mode = -1;                 /* -1 unknown, 0 off, else NP_ROLE_* */
static int s_started, s_dead, s_local_port = -1;
static np_session s_session;
static uint32_t s_frame;                /* updates run so far */
static uint32_t s_fight_steps;
static uint32_t s_seed_uses;
static char s_banner[160];

int netplay_on(void)
{
    if (s_mode < 0) {
        const char *e = getenv("RECOMP_NETPLAY");
        s_mode = !e || !*e ? 0 : (!strcmp(e, "host") || !strcmp(e, "1")) ? NP_ROLE_HOST
               : (!strcmp(e, "join") || !strcmp(e, "client")) ? NP_ROLE_CLIENT : 0;
    }
    return s_mode != 0 && !s_dead;
}

uint32_t netplay_frame(void) { return s_frame; }

/* A different, reproducible value for every seeding the title does: every machine performs
 * the same seedings in the same order, so the n-th one gets the same value everywhere. */
uint32_t netplay_seed(void)
{
    uint32_t x = s_session.seed ^ (++s_seed_uses * 0x9E3779B9u);
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

const char *netplay_banner(void) { return s_banner[0] ? s_banner : NULL; }

void netplay_prepare(void)
{
    if (!netplay_on()) return;
    /* The intro movies play against the host's clock; start every machine at the title. */
    if (!getenv("RECOMP_SKIP_INTRO")) _putenv_s("RECOMP_SKIP_INTRO", "1");
}

static int env_int(const char *name, int fallback)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

int netplay_start(void)
{
    np_config cfg;
    const char *e;
    if (!netplay_on()) return 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.role = s_mode;
    e = getenv("RECOMP_NETPLAY_ADDR");
    if (e) snprintf(cfg.address, sizeof cfg.address, "%s", e);
    cfg.port = env_int("RECOMP_NETPLAY_PORT", NP_DEFAULT_PORT);
    cfg.players = env_int("RECOMP_NETPLAY_PLAYERS", 2);
    cfg.delay = env_int("RECOMP_NETPLAY_DELAY", 0);
    cfg.blob_size = BLOB_BYTES;
    s_local_port = env_int("RECOMP_NETPLAY_LOCAL_PORT", -1);
    e = getenv("RECOMP_NETPLAY_NAME");
    snprintf(cfg.name, sizeof cfg.name, "%s", e && *e ? e : (s_mode == NP_ROLE_HOST ? "Host" : "Player"));
#ifdef DEFJAM_VERSION
    snprintf(cfg.content_id, sizeof cfg.content_id, "djffny-netplay-3/%s", DEFJAM_VERSION);
#else
    snprintf(cfg.content_id, sizeof cfg.content_id, "djffny-netplay-3/dev");
#endif
    if (s_mode == NP_ROLE_CLIENT && !cfg.address[0]) {
        fprintf(stderr, "[NETPLAY] RECOMP_NETPLAY=join needs RECOMP_NETPLAY_ADDR (the host's IP)\n");
        s_dead = 1;
        return -1;
    }
    /* Everyone's profiles into one save area for the session (netplay_lobby.h), unless
     * RECOMP_NETPLAY_SHARE_PROFILES=0. main.c then points the save area at that copy. */
    e = getenv("RECOMP_NETPLAY_SHARE_PROFILES");
    if (!e || *e != '0') {
        const char *data = getenv("DEFJAM_DATA");
        char mine[1024], session_save[1024], session_user[1024];
        snprintf(mine, sizeof mine, "%s/save/UserData/45410049", data && *data ? data : ".");
        snprintf(session_save, sizeof session_save, "%s/netplay/save", data && *data ? data : ".");
        snprintf(session_user, sizeof session_user, "%s/UserData/45410049", session_save);
        np_say("Netplay: %s", s_mode == NP_ROLE_HOST ? "hospedando" : "uniendose");
        if (np_lobby_run(s_mode == NP_ROLE_HOST, cfg.address, cfg.port, cfg.players, cfg.content_id,
                         cfg.name, mine, session_user)) {
            np_say("No se pudo armar la partida: %s", np_lobby_error());
            np_say("El juego sigue sin netplay.");
            s_dead = 1;
            return -1;
        }
        _putenv_s("RECOMP_NETPLAY_SAVE_DIR", session_save);
    }
    if (np_start(&cfg)) {
        np_say("No se pudo iniciar el netplay: %s", np_status());
        s_dead = 1;
        return -1;
    }
    snprintf(s_banner, sizeof s_banner, "Netplay: %s", np_status());
    fflush(stderr);
    return 0;
}

/* ---- the step gate ------------------------------------------------------------------- */

static void read_port(uint32_t staging, int port, uint8_t *blob)
{
    uint32_t status = MEM32(staging + (uint32_t)port * 4u);
    memcpy(blob, &status, 4);
    memcpy(blob + 4, (const void *)XBOX_PTR(staging + 0x10u + (uint32_t)port * REPORT_BYTES), REPORT_BYTES);
}

static void write_port(uint32_t staging, int port, const uint8_t *blob)
{
    uint32_t status;
    memcpy(&status, blob, 4);
    if (status != 2u) {                 /* no controller: as the worker writes it, zeroed */
        MEM32(staging + (uint32_t)port * 4u) = 1u;
        memset((void *)XBOX_PTR(staging + 0x10u + (uint32_t)port * REPORT_BYTES), 0, REPORT_BYTES);
        return;
    }
    MEM32(staging + (uint32_t)port * 4u) = 2u;
    memcpy((void *)XBOX_PTR(staging + 0x10u + (uint32_t)port * REPORT_BYTES), blob + 4, REPORT_BYTES);
}

static void local_input(uint32_t staging, uint8_t *blob)
{
    int port = s_local_port;
    if (port < 0 || port >= PORTS) {
        for (port = 0; port < PORTS; port++)
            if (MEM32(staging + (uint32_t)port * 4u) == 2u) break;
    }
    if (port >= PORTS) {                /* no controller here: send "absent" */
        uint32_t absent = 1u;
        memset(blob, 0, BLOB_BYTES);
        memcpy(blob, &absent, 4);
        return;
    }
    read_port(staging, port, blob);
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* State that must agree on every machine, without pointers (heap addresses differ). */
static uint32_t state_hash(void)
{
    uint32_t h = 2166136261u, v, slot;
    v = MEM32(CLOGIC_STATE); h = fnv(h, &v, 4);
    v = MEM32(G_PHASE); h = fnv(h, &v, 4);
    v = MEM32(G_RNG_STATE); h = fnv(h, &v, 4);
    v = MEM32(G_RNG_INC); h = fnv(h, &v, 4);
    for (slot = 0; slot < 4; slot++) {
        uint32_t a = ACTORS + slot * ACTOR_STRIDE;
        if (MEM32(a + F_SLOT) == 0xFFFFFFFFu) continue;
        h = fnv(h, (const void *)XBOX_PTR(a + F_CHARACTER), 8);         /* character, state */
        h = fnv(h, (const void *)XBOX_PTR(a + F_POS_X), 12);            /* position */
        h = fnv(h, (const void *)XBOX_PTR(a + F_FACING), 4);
        h = fnv(h, (const void *)XBOX_PTR(a + F_HEALTH), 4);
        v = MEM32(AI_GEN_STATE + slot * 0x88u); h = fnv(h, &v, 4);
    }
    return h;
}

static void begin_session(void)
{
    int r;
    uint32_t f;
    uint8_t absent[BLOB_BYTES];
    np_say("Arrancando la sesion...");
    while ((r = np_wait_session(1000, &s_session)) == 0) {
        snprintf(s_banner, sizeof s_banner, "Netplay: %s", np_status());
        fflush(stderr);
    }
    if (r < 0) {
        fprintf(stderr, "[NETPLAY] no session (%s); playing locally\n", np_status());
        s_dead = 1;
        return;
    }
    /* The first `delay` updates have no input from anyone yet: every port starts empty. */
    memset(absent, 0, sizeof absent);
    absent[0] = 1;
    for (f = 0; f < (uint32_t)s_session.delay; f++) np_submit(f, absent);
    s_started = 1;
    snprintf(s_banner, sizeof s_banner, "Netplay: player %d of %d, delay %d",
             s_session.player + 1, s_session.players, s_session.delay);
    np_say("%s. A jugar! (semilla %08X)", s_banner, s_session.seed);
}

/* sub_000D75E0 said "run an update" and has just staged its record. */
void netplay_step_gate(uint32_t queue)
{
    uint8_t mine[BLOB_BYTES], all[NP_MAX_PLAYERS * BLOB_BYTES];
    uint32_t staging = queue + STAGING;
    int r, port, waited = 0;
    if (!netplay_on()) return;
    if (!s_started) {
        begin_session();
        if (!s_started) return;
    }
    if (s_frame % 60u == 0u) {
        static int trace = -1;
        uint32_t h = state_hash();
        if (trace < 0) { const char *e = getenv("RECOMP_NETPLAY_TRACE"); trace = e && *e == '1'; }
        if (trace) fprintf(stderr, "[NETPLAY-HASH] %u %08X clogic=%u phase=%u rng=%08X\n", s_frame, h,
                           MEM32(CLOGIC_STATE), MEM32(G_PHASE), MEM32(G_RNG_STATE));
        np_report_hash(s_frame, h);
    }
    local_input(staging, mine);
    np_submit(s_frame + (uint32_t)s_session.delay, mine);
    while ((r = np_get(s_frame, all, 250)) == 0) {
        if (++waited % 8 == 0) {
            fprintf(stderr, "[NETPLAY] waiting for the other players at update %u (%s)\n", s_frame, np_status());
            fflush(stderr);
        }
    }
    if (r < 0) {
        np_say("Se acabo la sesion: %s", np_status());
        snprintf(s_banner, sizeof s_banner, "Netplay ended: %s", np_status());
        fflush(stderr);
        s_dead = 1;
        return;
    }
    for (port = 0; port < PORTS; port++) {
        if (port < s_session.players) write_port(staging, port, all + (size_t)port * BLOB_BYTES);
        else {
            static const uint8_t none[BLOB_BYTES] = {1};
            write_port(staging, port, none);
        }
    }
    if ((s_frame & 0x3FFu) == 0 || (np_desync_frame() >= 0 && !strstr(s_banner, "DESYNC"))) {
        long long d = np_desync_frame();
        if (d >= 0) snprintf(s_banner, sizeof s_banner, "Netplay: DESYNC at update %lld", d);
        else snprintf(s_banner, sizeof s_banner, "Netplay: player %d of %d, ping %d ms, delay %d",
                      s_session.player + 1, s_session.players, np_rtt_ms(), s_session.delay);
        fprintf(stderr, "[NETPLAY] update %u: %s\n", s_frame, s_banner);
        fflush(stderr);
    }
    s_frame++;
}

/* sub_00087700: one live fight step is about to run. */
void netplay_fight_step(void)
{
    if (!netplay_on() || !s_started) return;
    s_fight_steps++;
    MEM32(SPECTATOR_COUNT) = 0;
    MEM32(G_RNG_UTIL) = s_fight_steps * 2654435761u + (s_session.seed | 1u);
}

/* sub_001A4D90: a match starts. */
void netplay_match_start(void)
{
    s_fight_steps = 0;
}
