/* netplay_net.h -- the network half of netplay: a lockstep input exchange over UDP.
 *
 * Game-agnostic. Every machine runs the same game; what travels is each player's
 * input for each simulation frame, as an opaque blob of a fixed size. The host
 * collects every player's blob for a frame and sends the whole set (a "bundle")
 * to every client; each machine applies frame N's bundle at its update N. Star
 * topology, so clients only need to reach the host (a Radmin VPN address works
 * like a LAN address).
 *
 * Everything runs on one network thread of its own, so a machine that is busy
 * loading keeps answering. The game thread only calls np_submit / np_get.
 */
#ifndef DEFJAM_NETPLAY_NET_H
#define DEFJAM_NETPLAY_NET_H

#include <stdint.h>

#define NP_MAX_PLAYERS   4
#define NP_BLOB_MAX      256
#define NP_DEFAULT_PORT  27015
#define NP_NAME_MAX      24
#define NP_ID_MAX        64

enum { NP_ROLE_OFF = 0, NP_ROLE_HOST = 1, NP_ROLE_CLIENT = 2 };

typedef struct np_config {
    int role;                       /* NP_ROLE_HOST or NP_ROLE_CLIENT */
    char address[128];              /* client: the host's address (name or IPv4) */
    int port;                       /* UDP port; 0 = NP_DEFAULT_PORT */
    int players;                    /* host: how many players the session waits for (2..4) */
    int delay;                      /* frames of input delay; 0 = choose from the measured ping */
    unsigned blob_size;             /* bytes of input per player per frame (<= NP_BLOB_MAX) */
    char content_id[NP_ID_MAX];     /* must be identical on every machine (build, dump, rules) */
    char name[NP_NAME_MAX];         /* shown to the others */
} np_config;

typedef struct np_session {
    int player;                     /* this machine's player index (host = 0) */
    int players;
    int delay;
    uint32_t seed;                  /* shared by every machine, chosen by the host */
    int rtt_ms;                     /* worst round trip the host measured before starting */
    char names[NP_MAX_PLAYERS][NP_NAME_MAX];
} np_session;

/* Opens the socket and starts the network thread. 0 on success. */
int np_start(const np_config *cfg);

/* Blocks until the host has started the session (all players joined), or the
 * timeout passes (-1 = wait forever). 1 started, 0 timed out, -1 failed (see np_status). */
int np_wait_session(int timeout_ms, np_session *out);

/* This machine's input for a frame. Frames must be submitted in order, with no gaps. */
void np_submit(uint32_t frame, const void *blob);

/* Every player's input for a frame, player 0 first (players * blob_size bytes).
 * 1 = ready, 0 = not yet after timeout_ms, -1 = the session is over. */
int np_get(uint32_t frame, void *blobs, int timeout_ms);

/* A digest of the game state after a frame, for desync detection. Clients send
 * theirs to the host, which compares them with its own. */
void np_report_hash(uint32_t frame, uint32_t hash);

/* The first frame at which a client's digest differed from the host's, or -1. */
long long np_desync_frame(void);

/* Smoothed round trip to the host (clients) or the worst client (host), ms. */
int np_rtt_ms(void);

/* A one-line human-readable state ("waiting for 1 player", "connected", an error). */
const char *np_status(void);

int np_active(void);
void np_stop(void);

#endif
