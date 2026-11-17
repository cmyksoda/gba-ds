/* What the 3DS itself gives GBA DS besides SDL's part (sdl_ctr.c): its log,
 * its model, its memory, its cores - and what armGBA asks of a 3DS host
 * (jitmem.c). */
#ifndef GBADS_CTR_HOST_H
#define GBADS_CTR_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* armGBA's names for them (its 3DS host code calls these). */
void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
bool isEmulator(void); /* Azahar and Citra */
bool isNew3ds(void);

size_t host_heap_free(void);
/* A ROM at the top of the heap's range, out of the way of the heap's own allocations (NULL: no room). */
uint8_t* host_top_alloc(size_t size);
bool host_top_free(void* p); /* false: not one of those */
bool host_top_owns(const void* p);
void host_rom_free(void* p); /* either kind */
/* Memory the GPU can read (the game's picture), 128-byte aligned. */
void* host_linear_alloc(size_t size);
void host_linear_free(void* p);
/* The core the app's side work runs on (the hidden copy of the game that
 * draws the second screen, a logo's extraction, files): the New 3DS's
 * fourth (side_core in gba-ds.cfg), else whichever the system gives. */
int host_side_core(void);
/* The core the next pthread_create puts its thread on (the New 3DS's third, 2, for the game's own
 * frames), once; the side core after. */
#define HOST_CORE_AUTO (-100)
void host_next_thread_core(int core);
/* The calling thread's core, and the instruction cache of that core emptied (each core has its own, and code another core
 * wrote is what it may still hold from before: armgba_icache=0 turns the emptying off, to compare; false then). */
int host_core_now(void);
bool host_icache_invalidate(void);
/* The name the next thread made is listed under (host_threads_describe), once. */
void host_next_thread_name(const char* name);
/* The bench: a job begins (ended threads are forgotten) / one line with the core each thread found itself on and
 * whether the core asked for was refused. */
void host_threads_job_start(void);
void host_threads_describe(char* out, size_t n);
/* The time sdl_ctr.c's drawing has waited for the GPU, in SDL_GetPerformanceCounter ticks (a sum that wraps). */
uint32_t host_gpu_wait_ticks(void);
/* gba-ds.cfg's side_core, capture_core, ahead_core, video_core (-1: as built in), core1_limit (a thread on
 * core 1 needs the app's share of it set) and core_probe; at the start, and again for each game. */
void host_set_cores(int side, int capture, int ahead, int video, int core1Percent, bool probe);
/* gba-ds.cfg's video_wake: mGBA's drawing thread is woken when a line number's low bits (this mask: 0, 1, 3, 7, 15, 31, 63) are all set. */
void host_set_video_wake(int mask);
/* What to give host_next_thread_core for the map capture's thread, and for the Frame Buffer's (mGBA's
 * drawing thread on or not). */
int host_capture_core(void);
int host_ahead_core(bool threadedVideo);
/* The next machine made is the game itself (the large share of the
 * recompiler's memory), or a copy. */
void host_log_checkpoint(void);
/* The log (stderr) is written to log.txt on a thread of its own; this waits until it all is (not for play). */
void host_log_flush(void);
/* The bench's lines go through the same thread: put one (write is called with it, on the thread), wait for all that
 * were put, stop the thread, and the longest write in ms since the last call. */
void host_line_put(const char* line, void (*write)(const char* line));
/* lines_first: the writer thread at the game's priority instead of a step below it (above the hidden copy's thread). */
void host_set_lines_first(bool on);
void host_line_drain(void);
void host_line_stop(void);
double host_line_worst_ms(void);
void host_ui_busy(void);
void host_fs_yield(void);
void host_mem_note(const char* what);
void host_jit_main(bool main, size_t romToLoad, bool hiddenCopy);
/* A folder on the SD card listed in one go: its entries (malloc'd, the
 * caller frees them), each with whether it's a folder and a file's size.
 * Returns how many, or -1 if the folder can't be read. */
typedef struct {
	char name[256];
	bool dir;
	long long size;
} HostEntry;
int host_read_dir(const char* path, HostEntry** out);
/* The first n bytes of a file on the SD card, with as few calls on it as
 * that takes. */
bool host_read_head(const char* path, void* out, size_t n);
/* A bench build: a job's results to the PC (a LOG_URL build) as bench-TAG-WHAT. */
void host_bench_report(const char* path, const char* what);
/* TEST ONLY (a SLOWSD=1 build): the SD card freezes on a schedule (slow-sd.txt) while on. */
void host_slow_sd(bool on);
/* Before the app exits. */
void jitHostRestoreMemory(void);

#endif
