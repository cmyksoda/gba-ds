/* The test bench: see bench.h. A jobs file holds lines
 *
 *   job ID | ROM FILE | SCRIPT | FRAMES
 *
 * the ROM in rom_dir, the script (an input script, its ticks the game's frames) beside the jobs file. Each job runs
 * with the bench's own saves/ and states/ folders beside the jobs file, never the player's, starting with no battery
 * save; results go to stderr and to results.txt there. */
#include "bench.h"

#include "emu.h"
#include "game.h"
#include "profiles.h"
#include "paths.h"
#include "ui.h"
#ifdef __3DS__
#include "host.h"
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
	double sum, worst, emu, draw, screen;
	int busy;
	uint64_t first;
} w;

bool bench_active(void) {
	return active;
}

static void put(const char* line) {
	fprintf(stderr, "%s\n", line);
	if (results) {
		fprintf(results, "%s\n", line);
		fflush(results);
	}
	if (jobResults) fprintf(jobResults, "%s\n", line);
}

void bench_frame(uint64_t frame, double emuMs, double drawMs, double screenMs, bool captureBusy) {
	if (!active) return;
	++jobFrames;
	Uint64 now = SDL_GetPerformanceCounter();
	if (!lastT) {
		lastT = now;
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
	w.busy += captureBusy;
	if (w.n == WINDOW) {
		char line[256];
		snprintf(line, sizeof(line), "bench %s frames %llu-%llu: %.2f ms, worst %.2f, emulating %.2f, drawing %.2f, capture busy %d%%, second screen %.2f",
		         jobId, (unsigned long long) w.first, (unsigned long long) frame, w.sum / w.n, w.worst, w.emu / w.n, w.draw / w.n,
		         w.busy * 100 / w.n, w.screen / w.n);
		put(line);
		memset(&w, 0, sizeof(w));
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
		char* f[4] = { 0 };
		char* p = s + 4;
		for (int i = 0; i < 4 && p; ++i) {
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
#ifdef __3DS__
		host_mem_note("a bench job's start"); /* (a leak from one game to the next shows here) */
#endif
		ui_logo_prepare(cfg, cfg->rom); /* (made before the timing: on the side core it would weigh on the captures) */
		snprintf(path, sizeof(path), "%s/job-%s.txt", dir, jobId);
		jobResults = fopen(path, "w");
		if (jobResults) fprintf(jobResults, "bench start: GBA DS %s, job %s\n", GBADS_VERSION, jobId);
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
			snprintf(msg, sizeof(msg), "bench %s start: %s, %d frames, profile: %s", jobId, f[1], cfg->quitAfter,
			         profile_find(code) ? code : "none (one screen and the logo in this build)");
		}
		put(msg);
		game_run(cfg, cfg->rom, false);
		/* (the script's end quits the game: the next job; a quit before it ends the bench) */
		if (ui_ticks() >= (uint64_t) cfg->quitAfter) uiQuit = 0;
		if (!jobFrames) {
			snprintf(msg, sizeof(msg), "bench %s failed: the game didn't start (the app's log says why)", jobId);
			put(msg);
		}
		snprintf(msg, sizeof(msg), "bench %s done", jobId);
		put(msg);
		++done;
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
	snprintf(path, sizeof(path), "%s/end.txt", dir);
	FILE* endFile = fopen(path, "w");
	if (endFile) {
		fprintf(endFile, "bench start: GBA DS %s\n%s\n", GBADS_VERSION, msg);
		fclose(endFile);
	}
#ifdef __3DS__
	host_bench_report(path, "end");
#endif
	if (results) fclose(results);
	results = NULL;
	active = false;
	return true;
}
