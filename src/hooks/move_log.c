/* Registro de golpes para aprender qué botones dan qué movimiento.
 *
 * Con RECOMP_MOVE_LOG=1, cada vez que un jugador humano conecta un golpe se escribe una línea
 * "[GOLPE]" en stderr con: el número del movimiento en la tabla WAZA (para buscarle el nombre),
 * su daño base, el estilo activo, la vida y el mareo del rival antes del golpe, y las teclas que el
 * jugador apretó en los 90 pasos anteriores (en las teclas por defecto del teclado). Solo lee
 * memoria: no cambia nada de la pelea ni del netplay. */
#include "move_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"

#define ACTORS     0x003B92E0u
#define STRIDE     0x12E8u
#define SLOTS      4
#define F_HEALTH   0x350u
#define F_DIZZY    0x374u
#define F_DIZZYMAX 0x378u
#define F_HELD     0x9E0u
#define F_PRESSED  0x9F0u
#define F_STYLE    0xAE4u
#define F_CPU      0x937u
#define F_MOVE     0xFB8u
#define HISTORY    24
#define WINDOW     90

static int s_on = -1;
static uint32_t s_step;
static uint32_t s_prev[SLOTS];
static struct { uint32_t step, bits; } s_hist[SLOTS][HISTORY];
static int s_head[SLOTS];
static uint32_t s_last_move[SLOTS], s_last_step[SLOTS];

static int on(void)
{
    if (s_on < 0) { const char *e = getenv("RECOMP_MOVE_LOG"); s_on = e && *e == '1'; }
    return s_on;
}

static float f32(uint32_t a) { float f; memcpy(&f, (const void *)XBOX_PTR(a), 4); return f; }

static void keys(uint32_t bits, char *out, size_t n)
{
    static const struct { uint32_t bit; const char *name; } map[] = {
        {0x1000u, "Q(poder)"}, {0x2000u, "E(bloqueo)"}, {0x20u, "K(agarre)"}, {0x10u, "Shift(correr)"},
        {0x80u, "I(puno)"}, {0x40u, "J(patada)"}, {0x1u, "W"}, {0x2u, "S"}, {0x4u, "A"}, {0x8u, "D"},
        {0x20000u, "flechas(blazin)"},
    };
    size_t i, len = 0;
    out[0] = 0;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        if (bits & map[i].bit)
            len += (size_t)snprintf(out + len, len < n ? n - len : 0, "%s%s", len ? "+" : "", map[i].name);
}

void move_log_step(void)
{
    int slot;
    if (!on()) return;
    s_step++;
    for (slot = 0; slot < SLOTS; slot++) {
        uint32_t a = ACTORS + (uint32_t)slot * STRIDE;
        uint32_t held = MEM32(a + F_HELD) & 0x330FFu;
        uint32_t fresh = held & ~s_prev[slot] & 0x30F0u;  /* botones recién apretados */
        if (fresh) {
            s_hist[slot][s_head[slot]].step = s_step;
            s_hist[slot][s_head[slot]].bits = held;        /* con lo que estaba sostenido */
            s_head[slot] = (s_head[slot] + 1) % HISTORY;
        }
        s_prev[slot] = held;
    }
}

void move_log_hit(uint32_t stack)
{
    uint32_t att = MEM32(stack + 4), def = MEM32(stack + 8), rec = MEM32(stack + 16);
    uint32_t aslot, dslot, move;
    char line[1024], k[160];
    int n, i;
    if (!on() || att < ACTORS || def < ACTORS || att >= ACTORS + SLOTS * STRIDE || def >= ACTORS + SLOTS * STRIDE)
        return;
    aslot = (att - ACTORS) / STRIDE; dslot = (def - ACTORS) / STRIDE;
    if (att != ACTORS + aslot * STRIDE || (MEM8(att + F_CPU) & 1)) return;   /* solo humanos */
    if (rec < 0x10000u || rec > 0x3FF0000u) return;
    move = MEM32(rec + 4);
    /* Las llaves repiten su "tic" muchas veces: una línea por llave basta. */
    if (move == s_last_move[aslot] && s_step - s_last_step[aslot] < 30) { s_last_step[aslot] = s_step; return; }
    s_last_move[aslot] = move; s_last_step[aslot] = s_step;
    n = snprintf(line, sizeof line, "[GOLPE] paso=%u jugador=%u estilo=%u mov=%u dano_base=%d rival_vida=%.1f rival_mareo=%.0f%% teclas:",
                 s_step, aslot + 1, MEM32(att + F_STYLE), move, (int)SMEM16(rec + 0x1C),
                 (double)f32(def + F_HEALTH),
                 f32(def + F_DIZZYMAX) > 0 ? 100.0 * f32(def + F_DIZZY) / f32(def + F_DIZZYMAX) : 0.0);
    for (i = 0; i < HISTORY && n < (int)sizeof line - 40; i++) {
        int idx = (s_head[aslot] + i) % HISTORY;
        uint32_t st = s_hist[aslot][idx].step;
        if (!st || s_step - st > WINDOW) continue;
        keys(s_hist[aslot][idx].bits, k, sizeof k);
        n += snprintf(line + n, sizeof line - (size_t)n, " [-%u %s]", s_step - st, k);
    }
    (void)dslot;
    fprintf(stderr, "%s\n", line);
}
