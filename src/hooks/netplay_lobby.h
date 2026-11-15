/* netplay_lobby.h -- before a netplay session starts: everyone's profiles into one save area.
 *
 * Each profile (a User ID, with its Story fighter) is one folder of the save area
 * (save/UserData/45410049/<folder>), and every copy of the port signs saves with the same key,
 * so a profile made on one PC loads on another. Before the game starts, every client sends its
 * profile folders to the host over TCP (same port number as the UDP session); the host merges
 * them with its own save area and sends the result back to everyone. Every machine then plays
 * the session on that merged copy, in a folder of its own: the players' real saves are not
 * touched, and every machine reads the same profiles, which the lockstep needs. */
#ifndef DEFJAM_NETPLAY_LOBBY_H
#define DEFJAM_NETPLAY_LOBBY_H

/* Runs the exchange. host: waits for players - 1 clients. client: connects to address.
 * my_userdata: this machine's save/UserData/45410049. session_userdata: where the merged copy goes.
 * 0 on success; on failure the reason is in np_lobby_error(). */
int np_lobby_run(int host, const char *address, int port, int players, const char *content_id,
                 const char *name, const char *my_userdata, const char *session_userdata);
const char *np_lobby_error(void);

/* A line for the player: the launching console window (Windows) and the log. */
void np_say(const char *fmt, ...);

/* Creates every missing directory of a path. */
int np_mkdirs(const char *path);

#endif
