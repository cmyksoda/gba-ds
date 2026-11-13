/* The 3DS's part of src/emu.c: one copy of a ROM in memory for every copy of the game playing it
 * (the game, the hidden copy, a logo's), read off the SD card in pieces. mGBA copies it only for a
 * copy that changes it (a profile's patch, a cartridge's clock or rumble). */
#include "host.h"

#include <mgba/core/core.h>
#include <mgba-util/vfs.h>

#include <3ds.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

uint8_t* emu_rom_read_all(const char* path, size_t* size);
bool emu_is_archive_name(const char* path);

typedef struct {
	char path[1024];
	uint8_t* rom;
	size_t size;
	int users;
} SharedRom;

enum { MAX_ROMS = 4, MAX_CORES = 8 };
static SharedRom roms[MAX_ROMS];
static struct {
	const struct mCore* core;
	int rom;
} holders[MAX_CORES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* (mGBA's config code makes its folder through this; mGBA's own 3DS app opens it at start) */
FS_Archive sdmcArchive;

/* A background thread's reading of a ROM can be given up part-way (a logo being made when a game
 * is started): the flag it set for itself. */
static _Thread_local const volatile bool* readCancel;
void emu_read_cancel(const volatile bool* flag) {
	readCancel = flag;
}

static uint8_t* read_rom(const char* path, size_t* n) {
	if (emu_is_archive_name(path)) return emu_rom_read_all(path, n);
	struct stat st;
	*n = 0;
	if (stat(path, &st) || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return NULL;
	FILE* f = fopen(path, "rb");
	uint8_t* p = f ? malloc((size_t) st.st_size) : NULL;
	if (!f) fprintf(stderr, "%s: can't be opened\n", path);
	else if (!p) fprintf(stderr, "%s: no memory for its %u KiB (%u KiB of the heap free)\n", path, (unsigned) (st.st_size >> 10), (unsigned) (host_heap_free() >> 10));
	/* (in pieces from a background thread: one read of a whole ROM holds the SD card for seconds,
	 * and the game list's own reads wait behind it) */
	bool side = threadGetCurrent() != NULL;
	size_t got = 0;
	while (p && got < (size_t) st.st_size) {
		size_t want = (size_t) st.st_size - got;
		if (side && want > 64 * 1024) want = 64 * 1024;
		if (readCancel && *readCancel) break;
		size_t r = fread(p + got, 1, want, f);
		if (!r) {
			fprintf(stderr, "%s: read stopped at %u of %u bytes\n", path, (unsigned) got, (unsigned) st.st_size);
			break;
		}
		got += r;
		if (side) host_fs_yield();
	}
	if (f) fclose(f);
	if (p && got == (size_t) st.st_size) {
		*n = got;
		return p;
	}
	free(p);
	return NULL;
}

/* (under the lock) */
static int find_rom(const char* path) {
	for (int i = 0; i < MAX_ROMS; ++i) {
		if (roms[i].rom && !strcmp(roms[i].path, path)) return i;
	}
	return -1;
}

static void drop_if_unused(int slot) {
	if (slot >= 0 && !roms[slot].users) {
		free(roms[slot].rom);
		roms[slot].rom = NULL;
	}
}

bool emu_ctr_load(struct mCore* core, const char* path) {
	pthread_mutex_lock(&lock);
	if (!sdmcArchive) FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
	int slot = find_rom(path);
	if (slot < 0) {
		/* (read outside the lock: another copy's start needn't wait for this one's SD card) */
		pthread_mutex_unlock(&lock);
		size_t size;
		uint8_t* rom = read_rom(path, &size);
		pthread_mutex_lock(&lock);
		if (!rom) {
			pthread_mutex_unlock(&lock);
			return false;
		}
		slot = find_rom(path);
		if (slot >= 0) {
			free(rom); /* (read twice at once: the first one kept) */
		} else {
			for (int i = 0; i < MAX_ROMS && slot < 0; ++i) {
				if (!roms[i].rom) slot = i;
			}
			if (slot >= 0) {
				snprintf(roms[slot].path, sizeof(roms[slot].path), "%s", path);
				roms[slot].rom = rom;
				roms[slot].size = size;
				roms[slot].users = 0;
			} else {
				free(rom);
			}
		}
	}
	int holder = -1;
	for (int i = 0; i < MAX_CORES && holder < 0; ++i) {
		if (!holders[i].core) holder = i;
	}
	if (slot < 0 || holder < 0) {
		drop_if_unused(slot);
		pthread_mutex_unlock(&lock);
		fprintf(stderr, "%s: too many copies open to share its ROM\n", path);
		return false;
	}
	struct VFile* vf = VFileFromConstMemory(roms[slot].rom, roms[slot].size);
	core->unloadROM(core);
	bool ok = vf && core->loadROM(core, vf);
	if (ok) {
		++roms[slot].users;
		holders[holder].core = core;
		holders[holder].rom = slot;
	} else if (vf) {
		vf->close(vf);
	}
	drop_if_unused(slot);
	pthread_mutex_unlock(&lock);
	return ok;
}

/* After the core is gone (mGBA keeps reading the ROM until then). */
void emu_ctr_unload(const struct mCore* core) {
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_CORES; ++i) {
		if (holders[i].core != core) continue;
		SharedRom* r = &roms[holders[i].rom];
		holders[i].core = NULL;
		if (--r->users <= 0) {
			free(r->rom);
			r->rom = NULL;
			r->users = 0;
		}
	}
	pthread_mutex_unlock(&lock);
}
