# Netplay (experimental)

Online play for up to four players, each on their own PC, over the internet or a virtual
LAN such as Radmin VPN or ZeroTier. Every machine runs the whole game from its own dump;
the only thing exchanged is each player's controller, once per simulation update
(lockstep, like Dolphin's netplay). Off unless `RECOMP_NETPLAY` is set.

## Use

Every player needs the **same build** (same source, same version) made from their own
USA Xbox dump, and the host's UDP port (27015 by default) must be reachable: with Radmin
VPN or ZeroTier that just means being in the same network. Allow the game through the
Windows firewall when it asks (on Radmin the network is often classed as *Public*: tick it).

* Host: `netplay\Netplay - Hospedar.bat`, enter the number of players (2-4).
* Everyone else: `netplay\Netplay - Unirse.bat`, enter the host's address (its Radmin IP).

Before the game starts, every player's profiles (User IDs, each with its Story fighter) are
gathered: clients send theirs to the host over TCP on the same port, the host merges them with its
own save area and sends the result back, and every machine plays the session on that copy
(`<data folder>\netplay\save`). Each player then picks their own profile in Battle. The players'
real saves are never touched, and nothing earned in a netplay session is kept. A profile whose
name is already in the session is left out. `RECOMP_NETPLAY_SHARE_PROFILES=0` turns this off
(then every machine uses its own save area, which must be identical).

The game starts on every machine once all players have joined. Player 1 is the host's
controller on port 1, player 2 the first to join on port 2, and so on. Each machine sends
its lowest connected controller port (`RECOMP_NETPLAY_LOCAL_PORT` picks another).

| Variable | Meaning |
|---|---|
| `RECOMP_NETPLAY` | `host` or `join` |
| `RECOMP_NETPLAY_ADDR` | join: the host's address |
| `RECOMP_NETPLAY_PORT` | UDP port, 27015 by default |
| `RECOMP_NETPLAY_PLAYERS` | host: players to wait for, 2-4 |
| `RECOMP_NETPLAY_DELAY` | input delay in updates; by default chosen from the ping (a full round trip plus two) |
| `RECOMP_NETPLAY_NAME` | name shown to the others |
| `RECOMP_NETPLAY_TRACE` | `1` logs a state hash every 60 updates (`[NETPLAY-HASH]`) |

## When something goes wrong

Every run keeps the game's log (`logs\run-*.log.err`) and the launcher's window (`logs\launcher-*.txt`).
Netplay always logs who joined, every wait over half a second, a traffic summary every ten seconds
(`[NETPLAY-STATS]`), a desync, and why a session ended.

The **a prueba de errores** launchers (`Netplay - Hospedar/Unirse (a prueba de errores).bat`,
`Jugar solo (a prueba de errores).bat`) also turn on the play-test recorder
([playtest-recorder.md](playtest-recorder.md): crash and hang bundles with a minidump, F9 to save the
last seconds) at 5 pictures a second, and netplay's diagnostics (`RECOMP_NETPLAY_DIAG=1`): the state
hash every 60 updates, every player's input (`logs\netplay-inputs-*.bin`), and on a desync a dump of
the compared state on every machine (`logs\netplay-desync-*.txt`) plus a recorder bundle.

`Recoger reporte.bat` zips the newest logs, netplay files, the newest recorder session (without its
sound), `settings.ini` and a description of the PC onto the desktop. It never includes game files or saves.

## How it works

* **Input.** The title's 60 Hz worker snapshots the four controller ports into a ring of
  records (four status words and four 0x94-byte reports); the step gate `sub_000D75E0`
  stages one record per simulation update, and that update reads its controllers from the
  staged record only. Netplay wraps the gate: when it says "run an update", this machine's
  controller goes out as its input for update N + delay, and the staged record is replaced
  by every player's input for update N, player P on port P. Menus, character select and
  fights all run on the same input everywhere.
* **Seeds.** Every place the title seeds a random generator from the time-stamp counter
  (the match generator and its increment, the four AI generators, the match logic's two
  generators, the general-purpose one) takes a value derived from a seed the host chooses.
* **Spectators.** The crowd animates with the host's frame timing and fighters react to it
  (`docs/research/deck-determinism-98.md`), so in netplay fighters are shown no spectators
  and the general-purpose generator restarts from the step number every step, as
  `RECOMP_TEST_ISOLATE_CROWD` does. Crowd interaction is therefore off in netplay.
* **Desync check.** Every 60 updates each machine hashes the match state (logic state,
  match generator, fighters' character, state, position, facing, health, AI generators);
  clients send theirs to the host, which compares. A difference is logged as `DESYNC`.
* **Network.** UDP, star topology: clients send their input to the host, the host sends
  every player's input to everyone. Each side resends what the other has not acknowledged,
  so lost packets cost latency, not correctness. See `src/hooks/netplay_net.c`.

## Limits

* Two to four players: the title has four controller ports and four fighter slots.
* Intro movies are skipped in netplay. Story mode is untested; use Battle.
* A desync is detected and reported, not repaired.
* Fighter mods, custom songs and language packs must be the same on every machine.
* Rumble follows the local port mapping.
