/* The test bench (bench = JOBS): every job starts a game from power-on, plays its input script unpaced and logs
 * frame times every 60 frames. */
#ifndef GBADS_BENCH_H
#define GBADS_BENCH_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>

/* Runs the jobs file cfg->bench; false if it can't be read. */
bool bench_run(Config* cfg);
bool bench_active(void);
/* Once a frame from the game loop: this frame's emulating and drawing times, and GBA DS's own work between them
 * (the second screen's map, HUD and snapshot pictures). */
void bench_frame(uint64_t frame, double emuMs, double drawMs, double screenMs, bool captureBusy);

#endif
