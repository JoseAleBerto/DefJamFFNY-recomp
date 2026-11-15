/* netplay.h -- lockstep netplay for Def Jam: Fight for NY (Recompiled).
 *
 * Every machine runs the whole game. The only thing exchanged is each player's
 * controller for each simulation update, through the network layer in
 * netplay_net.c. See docs/netplay.md.
 */
#ifndef DEFJAM_NETPLAY_H
#define DEFJAM_NETPLAY_H

#include <stdint.h>

/* Before settings are read: when RECOMP_NETPLAY is set, pins the switches that would make
 * two machines run differently (intro movies, launcher, frame pacing experiments). */
void netplay_prepare(void);

/* After settings are read: opens the session. Returns 0 when netplay is off or started. */
int netplay_start(void);

/* 1 while a session is running (or being set up). */
int netplay_on(void);

/* The seed every machine uses where the game would read the time-stamp counter. */
uint32_t netplay_seed(void);

/* Index of the update the game is running now (counts from 0 at the first update). */
uint32_t netplay_frame(void);

/* sub_000D75E0 (thiscall on the frame queue) returned "run an update": exchange and apply input. */
void netplay_step_gate(uint32_t queue);

/* Called once per live fight step (sub_00087700) and at match start (sub_001A4D90). */
void netplay_fight_step(void);
void netplay_match_start(void);

/* One line for the overlay / title bar ("Netplay: 2 players, 85 ms, delay 6"), or NULL. */
const char *netplay_banner(void);

#endif
