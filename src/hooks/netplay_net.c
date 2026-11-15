/* netplay_net.c -- see netplay_net.h.
 *
 * Wire format: every datagram starts with the 4-byte magic "DJNP", a protocol
 * version byte and a type byte. Integers are little-endian. Inputs travel as
 * records of one flag byte (0 = same as this player's previous record in the
 * same datagram, 1 = full blob follows), so a datagram is self-contained and a
 * lost one costs nothing but a resend: each side keeps sending everything the
 * other has not acknowledged yet, every few milliseconds.
 */
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
typedef SOCKET np_socket;
#  define NP_BAD_SOCKET INVALID_SOCKET
#  define np_close closesocket
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <pthread.h>
#  include <time.h>
typedef int np_socket;
#  define NP_BAD_SOCKET (-1)
#  define np_close close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay_net.h"

#define NP_MAGIC        0x504E4A44u      /* "DJNP" little-endian */
#define NP_VERSION      1
#define NP_RING         1024u            /* frames of input kept per player */
#define NP_MTU          1200             /* stay below Radmin/PPPoE fragmentation */
#define NP_TICK_MS      2
#define NP_RESEND_MS    12
#define NP_TIMEOUT_MS   20000
#define NP_HASH_RING    256u

enum {
    T_HELLO = 1, T_WELCOME, T_REJECT, T_PING, T_PONG, T_START, T_START_ACK,
    T_INPUT, T_BUNDLE, T_BYE
};

/* ---- small portability layer --------------------------------------------------- */

#if defined(_WIN32)
static CRITICAL_SECTION s_lock;
static void lock_init(void) { InitializeCriticalSection(&s_lock); }
static void lock(void) { EnterCriticalSection(&s_lock); }
static void unlock(void) { LeaveCriticalSection(&s_lock); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static uint64_t now_ms(void) { return GetTickCount64(); }
static HANDLE s_thread;
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock(void) { pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)(t.tv_nsec / 1000000);
}
static pthread_t s_thread;
#endif

typedef struct sockaddr_in np_addr;

static int addr_equal(const np_addr *a, const np_addr *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* ---- state ---------------------------------------------------------------------- */

typedef struct peer {
    int used;
    np_addr addr;
    char name[NP_NAME_MAX];
    uint32_t nonce;
    int started;                    /* acknowledged START (or sent input) */
    uint32_t their_next;            /* host: next input frame we still need from them */
    uint32_t acked_bundle;          /* host: they have every bundle frame below this */
    uint64_t last_heard, last_sent, ping_sent;
    uint32_t ping_token;
    int rtt;                        /* smoothed, ms; -1 = unknown */
    uint32_t sent_upto;
    uint32_t hash[256][2];          /* their digests: frame + 1, hash */
} peer;

static struct {
    int running, failed;
    char status[256];
    np_config cfg;
    np_socket sock;

    np_session session;
    int started;                    /* session parameters final, frames flow */

    /* inputs: valid[p][f % RING] holds the frame number + 1 when present */
    uint8_t blob[NP_MAX_PLAYERS][NP_RING][NP_BLOB_MAX];
    uint32_t valid[NP_MAX_PLAYERS][NP_RING];
    uint32_t local_next;            /* next frame this machine will submit */
    uint32_t complete;              /* every frame below this has all players' input here */

    /* host */
    peer peers[NP_MAX_PLAYERS];     /* index = player number; 0 unused (the host) */
    uint32_t start_token;
    uint32_t host_hash[NP_HASH_RING][2];    /* frame + 1, hash */

    /* client */
    np_addr host;
    uint32_t my_nonce;
    int welcomed;
    uint32_t host_needs;            /* host has every input of ours below this */
    uint64_t host_last_heard, last_hello, last_sent;
    int rtt;
    uint32_t pending_hash_frame, pending_hash;  /* latest digest to piggyback */

    long long desync;
    uint64_t test_drop_state;
    unsigned test_drop_percent;
} N;

static void set_status(const char *msg)
{
    snprintf(N.status, sizeof N.status, "%s", msg);
    fprintf(stderr, "[NETPLAY] %s\n", msg);
}

/* ---- byte packing ----------------------------------------------------------------- */

typedef struct { uint8_t *p, *end; int bad; } wbuf;
typedef struct { const uint8_t *p, *end; int bad; } rbuf;

static void w8(wbuf *b, uint32_t v) { if (b->p + 1 > b->end) { b->bad = 1; return; } *b->p++ = (uint8_t)v; }
static void w32(wbuf *b, uint32_t v) { w8(b, v); w8(b, v >> 8); w8(b, v >> 16); w8(b, v >> 24); }
static void wmem(wbuf *b, const void *m, size_t n)
{
    if (b->p + n > b->end) { b->bad = 1; return; }
    memcpy(b->p, m, n); b->p += n;
}
static void wstr(wbuf *b, const char *s, size_t n)
{
    char tmp[NP_ID_MAX + 1] = {0};
    if (n > NP_ID_MAX) n = NP_ID_MAX;
    strncpy(tmp, s, n);
    wmem(b, tmp, n);
}
static uint32_t r8(rbuf *b) { if (b->p + 1 > b->end) { b->bad = 1; return 0; } return *b->p++; }
static uint32_t r32(rbuf *b) { uint32_t v = r8(b); v |= r8(b) << 8; v |= r8(b) << 16; v |= r8(b) << 24; return v; }
static void rmem(rbuf *b, void *m, size_t n)
{
    if (b->p + n > b->end) { b->bad = 1; memset(m, 0, n); return; }
    memcpy(m, b->p, n); b->p += n;
}
static void rstr(rbuf *b, char *out, size_t n)
{
    rmem(b, out, n);
    out[n - 1] = 0;
}

static void begin(wbuf *b, uint8_t *buf, size_t size, int type)
{
    b->p = buf; b->end = buf + size; b->bad = 0;
    w32(b, NP_MAGIC); w8(b, NP_VERSION); w8(b, (uint32_t)type);
}

static void send_to(const np_addr *to, const uint8_t *buf, const wbuf *b)
{
    if (b->bad) return;
    if (N.test_drop_percent) {                      /* NP_TEST_DROP: lose some datagrams on purpose */
        N.test_drop_state = N.test_drop_state * 6364136223846793005ull + 1442695040888963407ull;
        if ((unsigned)((N.test_drop_state >> 33) % 100u) < N.test_drop_percent) return;
    }
    sendto(N.sock, (const char *)buf, (int)(b->p - buf), 0, (const struct sockaddr *)to, sizeof *to);
}

/* ---- input ring --------------------------------------------------------------- */

static int have(int player, uint32_t frame)
{
    return N.valid[player][frame % NP_RING] == frame + 1u;
}

static void store(int player, uint32_t frame, const uint8_t *blob)
{
    /* Never overwrite a frame we already hold: inputs are immutable once sent. */
    if (have(player, frame)) return;
    memcpy(N.blob[player][frame % NP_RING], blob, N.cfg.blob_size);
    N.valid[player][frame % NP_RING] = frame + 1u;
}

static void advance_complete(void)
{
    for (;;) {
        int p;
        for (p = 0; p < N.session.players; p++)
            if (!have(p, N.complete)) return;
        N.complete++;
    }
}

/* Records for frames [first, last): returns how many fitted. */
static uint32_t write_records(wbuf *b, const int *players, int nplayers, uint32_t first, uint32_t last)
{
    uint32_t f, n = 0;
    const uint8_t *prev[NP_MAX_PLAYERS] = {0};
    for (f = first; f < last; f++) {
        int i;
        size_t need = 0;
        for (i = 0; i < nplayers; i++) {
            const uint8_t *cur = N.blob[players[i]][f % NP_RING];
            need += 1 + ((prev[i] && !memcmp(prev[i], cur, N.cfg.blob_size)) ? 0 : N.cfg.blob_size);
        }
        if (b->p + need > b->end) break;
        for (i = 0; i < nplayers; i++) {
            const uint8_t *cur = N.blob[players[i]][f % NP_RING];
            if (prev[i] && !memcmp(prev[i], cur, N.cfg.blob_size)) w8(b, 0);
            else { w8(b, 1); wmem(b, cur, N.cfg.blob_size); }
            prev[i] = cur;
        }
        n++;
    }
    return n;
}

static int read_records(rbuf *b, const int *players, int nplayers, uint32_t first, uint32_t count)
{
    uint8_t prev[NP_MAX_PLAYERS][NP_BLOB_MAX];
    int have_prev[NP_MAX_PLAYERS] = {0};
    uint32_t k;
    for (k = 0; k < count; k++) {
        int i;
        for (i = 0; i < nplayers; i++) {
            uint32_t flag = r8(b);
            if (flag == 1) { rmem(b, prev[i], N.cfg.blob_size); have_prev[i] = 1; }
            else if (flag != 0 || !have_prev[i]) b->bad = 1;
            if (b->bad) return 0;
            store(players[i], first + k, prev[i]);
        }
    }
    return 1;
}

/* ---- host ------------------------------------------------------------------------ */

static int host_joined(void)
{
    int p, n = 1;
    for (p = 1; p < NP_MAX_PLAYERS; p++) n += N.peers[p].used;
    return n;
}

static void host_send_start(peer *pe)
{
    uint8_t buf[256];
    wbuf b;
    int p;
    begin(&b, buf, sizeof buf, T_START);
    w32(&b, N.start_token);
    w8(&b, (uint32_t)(pe - N.peers));
    w8(&b, (uint32_t)N.session.players);
    w8(&b, (uint32_t)N.session.delay);
    w32(&b, N.session.seed);
    w32(&b, (uint32_t)N.session.rtt_ms);
    for (p = 0; p < N.session.players; p++) wstr(&b, N.session.names[p], NP_NAME_MAX);
    send_to(&pe->addr, buf, &b);
}

static void host_try_start(uint64_t now)
{
    int p, worst = 0, all_measured = 1;
    if (N.started || host_joined() < N.cfg.players) return;
    for (p = 1; p < N.cfg.players; p++) {
        if (N.peers[p].rtt < 0) all_measured = 0;
        else if (N.peers[p].rtt > worst) worst = N.peers[p].rtt;
    }
    if (!all_measured) return;                      /* wait for one ping per client */
    N.session.players = N.cfg.players;
    N.session.player = 0;
    N.session.rtt_ms = worst;
    if (N.cfg.delay > 0) N.session.delay = N.cfg.delay;
    else {
        /* A client's input reaches the host in half a round trip and the bundle comes back in the
         * other half, so the delay must cover a whole round trip plus some jitter. */
        int d = (worst + 16) / 17 + 2;
        N.session.delay = d < 2 ? 2 : d > 12 ? 12 : d;
    }
    N.session.seed = (uint32_t)(now * 2654435761u) ^ 0x5EEDu;
    snprintf(N.session.names[0], NP_NAME_MAX, "%s", N.cfg.name);
    for (p = 1; p < N.session.players; p++) snprintf(N.session.names[p], NP_NAME_MAX, "%s", N.peers[p].name);
    N.start_token = (uint32_t)now | 1u;
    N.started = 1;
    {
        char msg[160];
        snprintf(msg, sizeof msg, "session started: %d players, delay %d frames, worst ping %d ms",
                 N.session.players, N.session.delay, worst);
        set_status(msg);
    }
}

static void host_ping(peer *pe, uint64_t now, unsigned every_ms)
{
    uint8_t pb[32]; wbuf q;
    if (pe->ping_sent && now - pe->ping_sent < every_ms) return;
    pe->ping_token = (uint32_t)now | 1u;
    begin(&q, pb, sizeof pb, T_PING); w32(&q, pe->ping_token);
    send_to(&pe->addr, pb, &q);
    pe->ping_sent = now;
}

static void host_send_bundles(uint64_t now)
{
    int p, i, everyone[NP_MAX_PLAYERS];
    for (i = 0; i < NP_MAX_PLAYERS; i++) everyone[i] = i;
    for (p = 1; p < NP_MAX_PLAYERS; p++) {
        peer *pe = &N.peers[p];
        uint8_t buf[NP_MTU];
        wbuf b;
        uint32_t first, n;
        int fresh, owed;
        if (!pe->used) continue;
        host_ping(pe, now, N.started ? 500 : 150);
        if (!N.started) continue;
        if (!pe->started) {
            if (now - pe->last_sent >= 100) { host_send_start(pe); pe->last_sent = now; }
            continue;
        }
        first = pe->acked_bundle;
        fresh = N.complete > pe->sent_upto;
        owed = first < N.complete && now - pe->last_sent >= NP_RESEND_MS;
        if (!fresh && !owed && now - pe->last_sent < 250) continue;
        begin(&b, buf, sizeof buf, T_BUNDLE);
        w32(&b, pe->their_next);
        w32(&b, (uint32_t)(N.desync < 0 ? 0xFFFFFFFFu : (uint32_t)N.desync));
        w32(&b, first);
        {
            uint8_t *count_at = b.p;
            w8(&b, 0);
            n = first < N.complete ? write_records(&b, everyone, N.session.players, first,
                                                   N.complete - first > 255 ? first + 255 : N.complete) : 0;
            *count_at = (uint8_t)n;
        }
        send_to(&pe->addr, buf, &b);
        pe->last_sent = now;
        pe->sent_upto = N.complete;
    }
}

static peer *host_peer_by_addr(const np_addr *from)
{
    int p;
    for (p = 1; p < NP_MAX_PLAYERS; p++)
        if (N.peers[p].used && addr_equal(&N.peers[p].addr, from)) return &N.peers[p];
    return NULL;
}

static void flag_desync(uint32_t frame, const char *who)
{
    char msg[128];
    if (N.desync >= 0) return;
    N.desync = frame;
    snprintf(msg, sizeof msg, "DESYNC at frame %u (%s differs from the host)", frame, who);
    set_status(msg);
}

/* A client's digest arrives: keep it, and compare it if the host already has its own. */
static void host_check_hash(peer *pe, uint32_t frame, uint32_t hash)
{
    uint32_t *mine = N.host_hash[frame % NP_HASH_RING];
    uint32_t *theirs = pe->hash[frame % NP_HASH_RING];
    theirs[0] = frame + 1u; theirs[1] = hash;
    if (mine[0] == frame + 1u && mine[1] != hash) flag_desync(frame, pe->name);
}

static void host_receive(const np_addr *from, rbuf *b, int type, uint64_t now)
{
    peer *pe = host_peer_by_addr(from);
    if (type == T_HELLO) {
        char id[NP_ID_MAX], name[NP_NAME_MAX];
        uint32_t nonce;
        rstr(b, id, NP_ID_MAX); rstr(b, name, NP_NAME_MAX); nonce = r32(b);
        if (b->bad) return;
        if (!pe) {
            uint8_t buf[128]; wbuf w;
            int p;
            const char *why = NULL;
            if (strcmp(id, N.cfg.content_id)) why = "different game build or settings";
            else if (N.started || host_joined() >= N.cfg.players) why = "session full";
            if (why) {
                begin(&w, buf, sizeof buf, T_REJECT); wstr(&w, why, NP_ID_MAX);
                send_to(from, buf, &w);
                return;
            }
            for (p = 1; p < NP_MAX_PLAYERS && N.peers[p].used; p++) {}
            pe = &N.peers[p];
            memset(pe, 0, sizeof *pe);
            pe->used = 1; pe->addr = *from; pe->nonce = nonce; pe->rtt = -1;
            snprintf(pe->name, NP_NAME_MAX, "%s", name);
            {
                char msg[160];
                snprintf(msg, sizeof msg, "%s joined as player %d (%d of %d)", name, p + 1, host_joined(), N.cfg.players);
                set_status(msg);
            }
        }
        {
            uint8_t buf[64]; wbuf w;
            begin(&w, buf, sizeof buf, T_WELCOME); w32(&w, nonce); w8(&w, (uint32_t)(pe - N.peers));
            send_to(from, buf, &w);
        }
        pe->last_heard = now;
        host_ping(pe, now, 150);
        return;
    }
    if (!pe) return;
    pe->last_heard = now;
    switch (type) {
    case T_PING: {
        uint8_t pb[32]; wbuf q; uint32_t t = r32(b);
        begin(&q, pb, sizeof pb, T_PONG); w32(&q, t);
        send_to(from, pb, &q);
        break;
    }
    case T_PONG: {
        uint32_t t = r32(b);
        if (!b->bad && t == pe->ping_token) {
            int r = (int)(now - pe->ping_sent);
            pe->rtt = pe->rtt < 0 ? r : (pe->rtt * 7 + r) / 8;
        }
        break;
    }
    case T_START_ACK:
        if (r32(b) == N.start_token) pe->started = 1;
        break;
    case T_INPUT: {
        int player = (int)(pe - N.peers);
        uint32_t acked = r32(b), hframe = r32(b), hash = r32(b), first = r32(b), count = r8(b);
        int who[1];
        if (b->bad || !N.started) break;
        pe->started = 1;
        if (acked > pe->acked_bundle && acked <= N.complete) pe->acked_bundle = acked;
        if (hframe != 0xFFFFFFFFu) host_check_hash(pe, hframe, hash);
        who[0] = player;
        if (count && first <= pe->their_next && first + count > pe->their_next) {
            if (read_records(b, who, 1, first, count)) {
                while (have(player, pe->their_next)) pe->their_next++;
                advance_complete();
            }
        }
        break;
    }
    case T_BYE: {
        char msg[96];
        snprintf(msg, sizeof msg, "%s left the session", pe->name);
        set_status(msg);
        if (N.started) N.failed = 1;
        else pe->used = 0;
        break;
    }
    default: break;
    }
}

/* ---- client ------------------------------------------------------------------------ */

static void client_send(uint64_t now)
{
    uint8_t buf[NP_MTU];
    wbuf b;
    if (!N.welcomed) {
        if (now - N.last_hello >= 250) {
            begin(&b, buf, sizeof buf, T_HELLO);
            wstr(&b, N.cfg.content_id, NP_ID_MAX); wstr(&b, N.cfg.name, NP_NAME_MAX); w32(&b, N.my_nonce);
            send_to(&N.host, buf, &b);
            N.last_hello = now;
        }
        return;
    }
    if (!N.started) return;
    {
        static uint32_t sent_upto;
        uint32_t first = N.host_needs, last = N.local_next, n;
        int me[1];
        int fresh = last > sent_upto;
        if (!fresh && now - N.last_sent < NP_RESEND_MS) return;
        if (first >= last && !fresh && now - N.last_sent < 250 && N.pending_hash_frame == 0xFFFFFFFFu) return;
        begin(&b, buf, sizeof buf, T_INPUT);
        w32(&b, N.complete);
        w32(&b, N.pending_hash_frame); w32(&b, N.pending_hash);
        w32(&b, first);
        {
            uint8_t *count_at = b.p;
            w8(&b, 0);
            me[0] = N.session.player;
            n = first < last ? write_records(&b, me, 1, first, last - first > 255 ? first + 255 : last) : 0;
            *count_at = (uint8_t)n;
        }
        send_to(&N.host, buf, &b);
        N.last_sent = now;
        sent_upto = last;
    }
}

static void client_receive(const np_addr *from, rbuf *b, int type, uint64_t now)
{
    if (!addr_equal(from, &N.host)) return;
    N.host_last_heard = now;
    switch (type) {
    case T_WELCOME:
        if (r32(b) == N.my_nonce && !N.welcomed) {
            char msg[96];
            N.welcomed = 1;
            N.session.player = (int)r8(b);
            snprintf(msg, sizeof msg, "connected to the host as player %d, waiting for the others", N.session.player + 1);
            set_status(msg);
        }
        break;
    case T_REJECT: {
        char why[NP_ID_MAX], msg[160];
        rstr(b, why, NP_ID_MAX);
        snprintf(msg, sizeof msg, "the host refused: %s", why);
        set_status(msg);
        N.failed = 1;
        break;
    }
    case T_PING: {
        uint8_t pb[32]; wbuf q; uint32_t t = r32(b);
        begin(&q, pb, sizeof pb, T_PONG); w32(&q, t);
        send_to(from, pb, &q);
        break;
    }
    case T_PONG: break;
    case T_START: {
        uint32_t token = r32(b);
        int me = (int)r8(b), players = (int)r8(b), delay = (int)r8(b), p;
        uint32_t seed = r32(b), rtt = r32(b);
        uint8_t ab[32]; wbuf a;
        if (b->bad || players < 2 || players > NP_MAX_PLAYERS || me < 1 || me >= players) break;
        if (!N.started) {
            char msg[160];
            N.session.player = me; N.session.players = players; N.session.delay = delay;
            N.session.seed = seed; N.session.rtt_ms = (int)rtt;
            for (p = 0; p < players; p++) rstr(b, N.session.names[p], NP_NAME_MAX);
            N.started = 1;
            snprintf(msg, sizeof msg, "session started: player %d of %d, delay %d frames", me + 1, players, delay);
            set_status(msg);
        }
        begin(&a, ab, sizeof ab, T_START_ACK); w32(&a, token);
        send_to(from, ab, &a);
        break;
    }
    case T_BUNDLE: {
        uint32_t needs = r32(b), desync = r32(b), first = r32(b), count = r8(b);
        int everyone[NP_MAX_PLAYERS], i;
        if (b->bad || !N.started) break;
        if (needs > N.host_needs && needs <= N.local_next) N.host_needs = needs;
        if (desync != 0xFFFFFFFFu && N.desync < 0) {
            char msg[96];
            N.desync = desync;
            snprintf(msg, sizeof msg, "DESYNC at frame %u (reported by the host)", desync);
            set_status(msg);
        }
        for (i = 0; i < N.session.players; i++) everyone[i] = i;
        if (count && first <= N.complete && first + count > N.complete) {
            /* Our own input comes back inside the bundle too; store() keeps what we already hold. */
            if (read_records(b, everyone, N.session.players, first, count)) advance_complete();
        }
        break;
    }
    case T_BYE:
        set_status("the host closed the session");
        N.failed = 1;
        break;
    default: break;
    }
}

/* ---- the thread ----------------------------------------------------------------------- */

static void pump(void)
{
    uint8_t buf[2048];
    for (;;) {
        np_addr from;
#if defined(_WIN32)
        int len = sizeof from;
#else
        socklen_t len = sizeof from;
#endif
        int n = (int)recvfrom(N.sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &len);
        rbuf b;
        uint64_t now;
        int type;
        if (n <= 0) break;
        b.p = buf; b.end = buf + n; b.bad = 0;
        if (r32(&b) != NP_MAGIC || r8(&b) != NP_VERSION) continue;
        type = (int)r8(&b);
        now = now_ms();
        lock();
        if (N.cfg.role == NP_ROLE_HOST) host_receive(&from, &b, type, now);
        else client_receive(&from, &b, type, now);
        unlock();
    }
}

static void check_timeouts(uint64_t now)
{
    if (N.cfg.role == NP_ROLE_HOST) {
        int p;
        for (p = 1; p < NP_MAX_PLAYERS; p++) {
            peer *pe = &N.peers[p];
            if (pe->used && pe->last_heard && now - pe->last_heard > NP_TIMEOUT_MS) {
                char msg[96];
                snprintf(msg, sizeof msg, "lost contact with %s", pe->name);
                set_status(msg);
                if (N.started) N.failed = 1;
                else pe->used = 0;
            }
        }
    } else if (N.welcomed && now - N.host_last_heard > NP_TIMEOUT_MS) {
        set_status("lost contact with the host");
        N.failed = 1;
    }
}

#if defined(_WIN32)
static DWORD WINAPI thread_main(LPVOID unused)
#else
static void *thread_main(void *unused)
#endif
{
    (void)unused;
    while (N.running) {
        uint64_t now;
        pump();
        now = now_ms();
        lock();
        if (!N.failed) {
            if (N.cfg.role == NP_ROLE_HOST) {
                host_try_start(now);
                if (N.started || host_joined() > 1) host_send_bundles(now);
            } else client_send(now);
            check_timeouts(now);
        }
        unlock();
        sleep_ms(NP_TICK_MS);
    }
    return 0;
}

/* ---- API ------------------------------------------------------------------------------- */

static int resolve(const char *name, int port, np_addr *out)
{
    struct addrinfo hints, *res = NULL;
    char service[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof service, "%d", port);
    if (getaddrinfo(name, service, &hints, &res) || !res) return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}

int np_start(const np_config *cfg)
{
    np_addr bind_addr;
    int port;
    const char *drop;
    if (N.running) return 0;
    memset(&N, 0, sizeof N);
    N.cfg = *cfg;
    N.desync = -1;
    N.rtt = -1;
    N.pending_hash_frame = 0xFFFFFFFFu;
    if (N.cfg.blob_size == 0 || N.cfg.blob_size > NP_BLOB_MAX) { set_status("bad input size"); return -1; }
    if (N.cfg.players < 2) N.cfg.players = 2;
    if (N.cfg.players > NP_MAX_PLAYERS) N.cfg.players = NP_MAX_PLAYERS;
    if (!N.cfg.name[0]) snprintf(N.cfg.name, NP_NAME_MAX, "%s", cfg->role == NP_ROLE_HOST ? "host" : "player");
    port = N.cfg.port ? N.cfg.port : NP_DEFAULT_PORT;
    drop = getenv("NP_TEST_DROP");
    if (drop && *drop) { N.test_drop_percent = (unsigned)atoi(drop); N.test_drop_state = (uint64_t)port * 977u + (uint64_t)cfg->role; }
    lock_init();
#if defined(_WIN32)
    {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w)) { set_status("could not start Winsock"); return -1; }
    }
#endif
    N.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (N.sock == NP_BAD_SOCKET) { set_status("could not open a UDP socket"); return -1; }
    {
        int big = 1 << 20;
        setsockopt(N.sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
    }
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((unsigned short)(cfg->role == NP_ROLE_HOST ? port : 0));
    if (bind(N.sock, (struct sockaddr *)&bind_addr, sizeof bind_addr)) {
        char msg[96];
        snprintf(msg, sizeof msg, "could not use UDP port %d (another program has it?)", port);
        set_status(msg);
        np_close(N.sock);
        return -1;
    }
#if defined(_WIN32)
    {
        u_long nb = 1;
        ioctlsocket(N.sock, FIONBIO, &nb);
    }
#else
    fcntl(N.sock, F_SETFL, fcntl(N.sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    if (cfg->role == NP_ROLE_CLIENT) {
        if (resolve(cfg->address, port, &N.host)) {
            char msg[192];
            snprintf(msg, sizeof msg, "cannot find the host '%s'", cfg->address);
            set_status(msg);
            np_close(N.sock);
            return -1;
        }
        N.my_nonce = (uint32_t)now_ms() ^ 0xC0FFEEu;
        N.host_last_heard = now_ms();
        {
            char msg[192];
            snprintf(msg, sizeof msg, "contacting the host %s:%d", cfg->address, port);
            set_status(msg);
        }
    } else {
        char msg[128];
        snprintf(msg, sizeof msg, "hosting on UDP port %d, waiting for %d more player(s)", port, N.cfg.players - 1);
        set_status(msg);
    }
    N.running = 1;
#if defined(_WIN32)
    s_thread = CreateThread(NULL, 0, thread_main, NULL, 0, NULL);
    if (!s_thread) { N.running = 0; set_status("could not start the network thread"); return -1; }
#else
    if (pthread_create(&s_thread, NULL, thread_main, NULL)) { N.running = 0; set_status("could not start the network thread"); return -1; }
#endif
    return 0;
}

int np_wait_session(int timeout_ms, np_session *out)
{
    uint64_t start = now_ms();
    for (;;) {
        int started, failed;
        lock();
        started = N.started; failed = N.failed;
        if (started && out) *out = N.session;
        unlock();
        if (failed) return -1;
        if (started) return 1;
        if (timeout_ms >= 0 && now_ms() - start >= (uint64_t)timeout_ms) return 0;
        sleep_ms(5);
    }
}

void np_submit(uint32_t frame, const void *blob)
{
    lock();
    if (N.started && frame == N.local_next) {
        store(N.session.player, frame, (const uint8_t *)blob);
        N.local_next++;
        if (N.cfg.role == NP_ROLE_HOST) advance_complete();
    }
    unlock();
}

int np_get(uint32_t frame, void *blobs, int timeout_ms)
{
    uint64_t start = now_ms();
    for (;;) {
        int ready, failed;
        lock();
        failed = N.failed;
        ready = N.started && frame < N.complete;
        if (ready) {
            int p;
            for (p = 0; p < N.session.players; p++)
                memcpy((uint8_t *)blobs + (size_t)p * N.cfg.blob_size, N.blob[p][frame % NP_RING], N.cfg.blob_size);
        }
        unlock();
        if (ready) return 1;
        if (failed) return -1;
        if (now_ms() - start >= (uint64_t)timeout_ms) return 0;
        sleep_ms(1);
    }
}

void np_report_hash(uint32_t frame, uint32_t hash)
{
    lock();
    if (N.cfg.role == NP_ROLE_HOST) {
        uint32_t *slot = N.host_hash[frame % NP_HASH_RING];
        int p;
        slot[0] = frame + 1u;
        slot[1] = hash;
        for (p = 1; p < NP_MAX_PLAYERS; p++) {
            uint32_t *theirs = N.peers[p].hash[frame % NP_HASH_RING];
            if (N.peers[p].used && theirs[0] == frame + 1u && theirs[1] != hash) flag_desync(frame, N.peers[p].name);
        }
    } else {
        N.pending_hash_frame = frame;
        N.pending_hash = hash;
    }
    unlock();
}

long long np_desync_frame(void)
{
    long long d;
    lock(); d = N.desync; unlock();
    return d;
}

int np_rtt_ms(void)
{
    int r = -1, p;
    lock();
    if (N.cfg.role == NP_ROLE_HOST) {
        for (p = 1; p < NP_MAX_PLAYERS; p++) if (N.peers[p].used && N.peers[p].rtt > r) r = N.peers[p].rtt;
    } else r = N.session.rtt_ms;
    unlock();
    return r;
}

const char *np_status(void) { return N.status; }
int np_active(void) { return N.running; }

void np_stop(void)
{
    uint8_t buf[16];
    wbuf b;
    int p;
    if (!N.running) return;
    lock();
    begin(&b, buf, sizeof buf, T_BYE);
    if (N.cfg.role == NP_ROLE_HOST) {
        for (p = 1; p < NP_MAX_PLAYERS; p++) if (N.peers[p].used) send_to(&N.peers[p].addr, buf, &b);
    } else send_to(&N.host, buf, &b);
    unlock();
    N.running = 0;
#if defined(_WIN32)
    WaitForSingleObject(s_thread, 1000);
#else
    pthread_join(s_thread, NULL);
#endif
    np_close(N.sock);
}
