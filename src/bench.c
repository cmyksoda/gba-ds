/* The test bench: see bench.h. A jobs file holds lines
 *
 *   job ID | ROM FILE | SCRIPT | FRAMES | OPTIONS
 *
 * the ROM in rom_dir, the script (an input script, its ticks the game's frames) beside the jobs file. Each job runs
 * with the bench's own saves/ and states/ folders beside the jobs file, never the player's, starting with no battery
 * save; results go to stderr and to results.txt there. OPTIONS (optional): gba-ds.cfg lines as key=value words, set
 * for this job alone (job ID@suffix: the same game run under other options). */
#include "bench.h"

#include "emu.h"
#include "game.h"
#include "mapcap.h"
#include "profiles.h"
#include "paths.h"
#include "ui.h"
#ifdef __3DS__
#include "host.h"
#include <pthread.h>
#include <time.h>
#endif

#include <SDL.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { WINDOW = 60 };

static bool active;
static FILE* results;
static FILE* jobResults; /* this job's lines alone (job-ID.txt): what a bench build posts after the job */
static char jobId[64];
static Uint64 lastT;
static unsigned long jobFrames; /* (none: the game didn't start) */
static struct {
	int n;
	double sum, worst, emu, draw, screen, pre, gap;
	int busy;
	uint64_t first;
} w;
/* what the other threads' sums stood at when this window began (the sums wrap: only differences count) */
typedef struct {
	uint32_t video, hiddenUs, hiddenFrames, gpu;
} Sums;
static Sums base;
static char threadsShown[512];

static void sums_read(Sums* s) {
#ifndef GBADS_ARMGBA
	s->video = emu_video_wait_us();
#else
	s->video = 0;
#endif
	mapcap_work_totals(&s->hiddenUs, &s->hiddenFrames);
#ifdef __3DS__
	s->gpu = host_gpu_wait_ticks();
#else
	s->gpu = 0;
#endif
}


bool bench_active(void) {
	return active;
}

static void write_line(const char* line) {
	fprintf(stderr, "%s\n", line);
	if (results) {
		fprintf(results, "%s\n", line);
		fflush(results);
	}
	if (jobResults) fprintf(jobResults, "%s\n", line);
}

#ifdef __3DS__
/* The lines are written by a thread of their own: the SD card can make a write wait for seconds, and
 * the console's first bench run lost 6.6 s to one every minute or so, in the game's frame. The
 * longest write of each window is in the next line (sd). */
enum { QUEUE = 64, LINE_MAX_LEN = 1280 };
static struct {
	pthread_mutex_t lock;
	/* (one thread to a condition variable: libctru's remembers a single wake, and a second waiter loses it) */
	pthread_cond_t wake, idle;
	pthread_t thread;
	bool started, quit;
	char lines[QUEUE][LINE_MAX_LEN];
	int head, count;
	double worstMs;
} q = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER };

static void* writer_main(void* arg) {
	(void) arg;
	pthread_mutex_lock(&q.lock);
	for (;;) {
		while (!q.count && !q.quit) pthread_cond_wait(&q.wake, &q.lock);
		if (!q.count) break;
		static char line[LINE_MAX_LEN];
		memcpy(line, q.lines[q.head], sizeof(line));
		pthread_mutex_unlock(&q.lock);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		write_line(line);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
		pthread_mutex_lock(&q.lock);
		if (ms > q.worstMs) q.worstMs = ms;
		q.head = (q.head + 1) % QUEUE;
		--q.count;
		pthread_cond_broadcast(&q.idle);
	}
	pthread_mutex_unlock(&q.lock);
	return NULL;
}

static void put(const char* line) {
	if (!q.started) q.started = pthread_create(&q.thread, NULL, writer_main, NULL) == 0;
	if (!q.started) {
		write_line(line);
		return;
	}
	pthread_mutex_lock(&q.lock);
	while (q.count == QUEUE) pthread_cond_wait(&q.idle, &q.lock);
	snprintf(q.lines[(q.head + q.count) % QUEUE], LINE_MAX_LEN, "%s", line);
	++q.count;
	pthread_cond_broadcast(&q.wake);
	pthread_mutex_unlock(&q.lock);
}

/* Everything put so far is written (a job's file is closed, or sent, after this). */
static void drain(void) {
	if (!q.started) return;
	pthread_mutex_lock(&q.lock);
	while (q.count) pthread_cond_wait(&q.idle, &q.lock);
	pthread_mutex_unlock(&q.lock);
}

static void stop_writer(void) {
	if (!q.started) return;
	pthread_mutex_lock(&q.lock);
	q.quit = true;
	pthread_cond_broadcast(&q.wake);
	pthread_mutex_unlock(&q.lock);
	pthread_join(q.thread, NULL);
	q.started = q.quit = false;
}

static double take_worst_write(void) {
	pthread_mutex_lock(&q.lock);
	double ms = q.worstMs;
	q.worstMs = 0;
	pthread_mutex_unlock(&q.lock);
	return ms;
}
#else
static void put(const char* line) {
	write_line(line);
}
static void drain(void) {}
static void stop_writer(void) {}
#endif

/* Which core each thread ran on (the console's; an emulator's processor ids mean nothing), once for a job, again if it
 * changed by the end. */
static void threads_line(void) {
#ifdef __3DS__
	char now[sizeof(threadsShown)];
	host_threads_describe(now, sizeof(now));
	if (!strcmp(now, threadsShown)) return;
	memcpy(threadsShown, now, sizeof(now));
	char line[sizeof(now) + 96];
	snprintf(line, sizeof(line), "bench %s threads: %s", jobId, now);
	put(line);
#endif
}

void bench_frame(uint64_t frame, double emuMs, double drawMs, double screenMs, bool captureBusy, double preMs, double loopMs) {
	if (!active) return;
	++jobFrames;
	Uint64 now = SDL_GetPerformanceCounter();
	if (!lastT) {
		lastT = now;
		sums_read(&base);
		return;
	}
	double ms = (now - lastT) * 1000.0 / SDL_GetPerformanceFrequency();
	lastT = now;
	if (!w.n) w.first = frame;
	++w.n;
	w.sum += ms;
	if (ms > w.worst) w.worst = ms;
	w.emu += emuMs;
	w.draw += drawMs;
	w.screen += screenMs;
	w.pre += preMs;
	w.gap += ms - loopMs;
	w.busy += captureBusy;
#ifdef __3DS__
	if (ms > 1000) {
		/* (a frame that stopped: where, as the game loop saw it) */
		char note[320];
		snprintf(note, sizeof(note), "bench %s stall at frame %llu: %.0f ms = before emulating %.0f + emulating %.0f + second screen %.0f + drawing %.0f + outside the loop %.0f",
		         jobId, (unsigned long long) frame, ms, preMs, emuMs, screenMs, drawMs, ms - loopMs);
		put(note);
	}
#else
	(void) preMs, (void) loopMs;
#endif
	if (w.n == WINDOW) {
		/* (the columns after "second screen" are appended: video wait is part of emulating, the others are not; pre, gap and sd, on the 3DS, come last) */
		char line[448];
		Sums s;
		sums_read(&s);
		int len = snprintf(line, sizeof(line),
		                   "bench %s frames %llu-%llu: %.2f ms, worst %.2f, emulating %.2f, drawing %.2f, capture busy %d%%, second screen %.2f",
		                   jobId, (unsigned long long) w.first, (unsigned long long) frame, w.sum / w.n, w.worst, w.emu / w.n,
		                   w.draw / w.n, w.busy * 100 / w.n, w.screen / w.n);
		len += snprintf(line + len, sizeof(line) - (size_t) len, ", video wait %.2f, hidden copy %.2f, hidden frames %u",
		                (s.video - base.video) / 1000.0 / w.n, (s.hiddenUs - base.hiddenUs) / 1000.0 / w.n,
		                (unsigned) (s.hiddenFrames - base.hiddenFrames));
#ifdef __3DS__
		len += snprintf(line + len, sizeof(line) - (size_t) len, ", gpu wait %.2f",
		                (s.gpu - base.gpu) * 1000.0 / SDL_GetPerformanceFrequency() / w.n);
		/* of the "other": before emulating, and outside the loop (its end, the lines written, the loop's start); sd: the longest write */
		snprintf(line + len, sizeof(line) - (size_t) len, ", pre %.2f, gap %.2f, sd %.1f", w.pre / w.n, w.gap / w.n, take_worst_write());
#endif
		put(line);
		base = s;
		memset(&w, 0, sizeof(w));
		if (!threadsShown[0]) threads_line();
	}
}

static char* trim(char* s) {
	while (*s == ' ' || *s == '\t') ++s;
	char* e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

/* The same folder, by its real path (either may not exist: then by name). */
static bool same_dir(const char* a, const char* b) {
	char ra[PATH_MAX], rb[PATH_MAX];
	if (!realpath(a, ra)) snprintf(ra, sizeof(ra), "%s", a);
	if (!realpath(b, rb)) snprintf(rb, sizeof(rb), "%s", b);
	size_t na = strlen(ra), nb = strlen(rb);
	while (na > 1 && ra[na - 1] == '/') ra[--na] = 0;
	while (nb > 1 && rb[nb - 1] == '/') rb[--nb] = 0;
	return !strcmp(ra, rb);
}

/* A job's OPTIONS on cfg: key=value words, the keys as gba-ds.cfg has them (a - is fine for a _). False, with the word
 * that isn't an option, if one isn't. */
static bool apply_options(Config* cfg, char* words, char* bad, size_t badSize) {
	for (char* word = strtok(words, " \t"); word; word = strtok(NULL, " \t")) {
		char* eq = strchr(word, '=');
		if (eq) *eq = 0;
		for (char* k = word; *k; ++k) {
			if (*k == '-') *k = '_';
		}
		if (!eq || !config_set(cfg, word, eq + 1)) {
			snprintf(bad, badSize, "%s", word);
			return false;
		}
	}
	return true;
}

bool bench_run(Config* cfg) {
	FILE* jobs = fopen(cfg->bench, "r");
	if (!jobs) {
		fprintf(stderr, "bench: can't read %s\n", cfg->bench);
		return false;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", cfg->bench);
	char* slash = strrchr(dir, '/');
	if (slash) *slash = 0;
	else snprintf(dir, sizeof(dir), ".");
	char saves[sizeof(cfg->saveDir)], states[sizeof(cfg->stateDir)], path[700], romDir[1024];
	snprintf(romDir, sizeof(romDir), "%s", cfg->romDir);
	if (strchr(romDir, ':')) *strchr(romDir, ':') = 0; /* (the first of the library's folders) */
	if (snprintf(saves, sizeof(saves), "%s/saves", dir) >= (int) sizeof(saves) ||
	    snprintf(states, sizeof(states), "%s/states", dir) >= (int) sizeof(states)) {
		fprintf(stderr, "bench: the jobs file's folder name is too long: not started\n");
		fclose(jobs);
		return false;
	}
	mkdirs(saves);
	mkdirs(states);
	if (same_dir(saves, romDir) || same_dir(states, romDir)) {
		fprintf(stderr, "bench: its folders would be the ROM folder %s: not started\n", romDir);
		fclose(jobs);
		return false;
	}
	/* (a fresh file each run, the last one kept beside it: a report never mixes two runs) */
	static char resultsPath[700];
	snprintf(resultsPath, sizeof(resultsPath), "%s/results.txt", dir);
	snprintf(path, sizeof(path), "%s/results-prev.txt", dir);
	remove(path);
	rename(resultsPath, path);
	results = fopen(resultsPath, "w");
	{
		char line[640];
		snprintf(line, sizeof(line), "bench start: GBA DS %s, jobs %s", GBADS_VERSION, cfg->bench);
		put(line);
	}
	/* the bench's own folders for everything a game writes; no resume point */
	snprintf(cfg->saveDir, sizeof(cfg->saveDir), "%s", saves);
	snprintf(cfg->stateDir, sizeof(cfg->stateDir), "%s", states);
	cfg->saveExt[0] = 0;
	cfg->saveFrom[0] = 0;
	cfg->autoSave = false;
	cfg->autoLoad = false;
	cfg->rewind = false;
	/* unpaced, presented without waiting for the screens: each frame as fast as it goes */
	cfg->vsync = false;
	cfg->matchDisplay = false;
	setenv("GBADS_NOPACE", "1", 1);
	active = true;
	char line[1024], msg[1200];
	int done = 0;
	while (!uiQuit && fgets(line, sizeof(line), jobs)) {
		char* s = trim(line);
		if (strncmp(s, "job ", 4)) continue;
		char* f[5] = { 0 };
		char* p = s + 4;
		for (int i = 0; i < 5 && p; ++i) {
			char* bar = strchr(p, '|');
			if (bar) *bar = 0;
			f[i] = trim(p);
			p = bar ? bar + 1 : NULL;
		}
		if (!f[3] || !*f[0] || !*f[1] || !*f[2] || atoi(f[3]) <= 0) {
			fprintf(stderr, "bench: a job line it can't read: %s\n", s);
			continue;
		}
		snprintf(jobId, sizeof(jobId), "%s", f[0]);
		if (snprintf(cfg->rom, sizeof(cfg->rom), "%s/%s", romDir, f[1]) >= (int) sizeof(cfg->rom)) {
			fprintf(stderr, "bench: %s: the ROM's path is too long, skipped\n", f[0]);
			continue;
		}
		/* this job's battery save only, in the bench's own folder: every job starts from a cartridge never played */
		{
			char base[512], sav[1200];
			snprintf(base, sizeof(base), "%s", f[1]);
			char* dot = strrchr(base, '.');
			if (dot) *dot = 0;
			snprintf(sav, sizeof(sav), "%s/%s.sav", saves, base);
			remove(sav);
			snprintf(sav, sizeof(sav), "%s/%s.srm", saves, base);
			remove(sav);
		}
		static Config before; /* (the config as the job found it: back when it has had options) */
		char options[256] = "", bad[64];
		if (f[4]) snprintf(options, sizeof(options), "%s", f[4]);
		if (f[4] && strlen(f[4]) >= sizeof(options)) {
			snprintf(msg, sizeof(msg), "bench %s failed: the job's options are longer than %d characters", jobId, (int) sizeof(options) - 1);
			put(msg);
			continue;
		}
		bool optioned = *options != 0;
		if (optioned) {
			char words[sizeof(options)];
			memcpy(words, options, sizeof(words));
			before = *cfg;
			if (!apply_options(cfg, words, bad, sizeof(bad))) {
				*cfg = before;
				snprintf(msg, sizeof(msg), "bench %s failed: no such option '%s' (the job's options: %s)", jobId, bad, options);
				put(msg);
				continue;
			}
		}
		ui_logo_prepare(cfg, cfg->rom); /* (made before the timing: on the side core it would weigh on the captures) */
		snprintf(path, sizeof(path), "%s/job-%s.txt", dir, jobId);
		jobResults = fopen(path, "w");
		if (jobResults) fprintf(jobResults, "bench start: GBA DS %s, job %s\n", GBADS_VERSION, jobId);
#ifdef __3DS__
		/* (in the job's own lines: a leak from one game to the next shows here) */
		snprintf(msg, sizeof(msg), "bench %s heap: %u KiB free at the start", jobId, (unsigned) (host_heap_free() >> 10));
		put(msg);
#endif
		snprintf(path, sizeof(path), "%s/%s", dir, f[2]);
		ui_script_restart(path);
		cfg->quitAfter = atoi(f[3]);
		memset(&w, 0, sizeof(w));
		lastT = 0;
		jobFrames = 0;
		{
			uint8_t h[0xB0];
			char code[5] = "????";
			if (emu_rom_header(cfg->rom, h, sizeof(h))) memcpy(code, h + 0xAC, 4);
			char opt[sizeof(options) + 16] = "";
			if (optioned) snprintf(opt, sizeof(opt), ", options: %s", options);
			snprintf(msg, sizeof(msg), "bench %s start: %s, %d frames%s, profile: %s", jobId, f[1], cfg->quitAfter, opt,
			         profile_find(code) ? code : "none (one screen and the logo in this build)");
		}
		put(msg);
		threadsShown[0] = 0;
#ifdef __3DS__
		host_threads_job_start();
#endif
		game_run(cfg, cfg->rom, false);
		/* (the script's end quits the game: the next job; a quit before it ends the bench) */
		if (ui_ticks() >= (uint64_t) cfg->quitAfter) uiQuit = 0;
		threads_line();
		if (optioned) *cfg = before;
#ifdef __3DS__
		snprintf(msg, sizeof(msg), "bench %s heap: %u KiB free at the end", jobId, (unsigned) (host_heap_free() >> 10));
		put(msg);
#endif
		if (!jobFrames) {
			snprintf(msg, sizeof(msg), "bench %s failed: the game didn't start (the app's log says why)", jobId);
			put(msg);
		}
		snprintf(msg, sizeof(msg), "bench %s done", jobId);
		put(msg);
		++done;
		drain();
		if (jobResults) fclose(jobResults);
		jobResults = NULL;
#ifdef __3DS__
		/* (a bench build: this job's results to the PC, on their own: a whole run's are too long to post) */
		snprintf(path, sizeof(path), "%s/job-%s.txt", dir, jobId);
		host_bench_report(path, jobId);
#endif
	}
	fclose(jobs);
	snprintf(msg, sizeof(msg), "bench end: %d job(s)", done);
	put(msg);
	drain();
	snprintf(path, sizeof(path), "%s/end.txt", dir);
	FILE* endFile = fopen(path, "w");
	if (endFile) {
		fprintf(endFile, "bench start: GBA DS %s\n%s\n", GBADS_VERSION, msg);
		fclose(endFile);
	}
#ifdef __3DS__
	host_bench_report(path, "end");
#endif
	stop_writer();
	if (results) fclose(results);
	results = NULL;
	active = false;
	return true;
}
