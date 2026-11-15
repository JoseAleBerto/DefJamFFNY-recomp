#ifndef DEFJAM_MOVE_LOG_H
#define DEFJAM_MOVE_LOG_H
#include <stdint.h>
/* Registro de golpes (RECOMP_MOVE_LOG=1): por cada golpe de un jugador humano escribe en stderr
 * qué movimiento salió, su daño base, el mareo del rival y los botones que apretó antes. */
void move_log_step(void);
void move_log_hit(uint32_t stack);
#endif
