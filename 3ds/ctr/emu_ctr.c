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
#include <unistd.h>

uint8_t* emu_rom_read_all(const char* path, size_t* size);
bool emu_is_archive_name(const char* path);

typedef struct {
	char path[1024];
	uint8_t* rom;
	size_t size;
	bool top; /* at the top of the heap's range (top_alloc), else malloc'd */
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

/* ROMs at the top of the heap's address range, the heap's end brought down below them: the heap's
 * own allocations stay underneath, so a 32 MiB ROM finds one stretch however the heap is cut up. */
extern char* fake_heap_end;
extern size_t hostTopBytes;
void __malloc_lock(struct _reent* r);
void __malloc_unlock(struct _reent* r);
enum { TOP_BLOCKS = 4, HEAP_ROOM = 4 << 20 /* (always left for the heap to grow into) */ };
static struct {
	uint8_t* at;
	size_t size;
} topBlocks[TOP_BLOCKS];
static char* topEnd; /* the heap's end before any ROM came */

/* (under the malloc lock: sbrk reads fake_heap_end) */
static void top_end_update(void) {
	char* low = topEnd;
	for (int i = 0; i < TOP_BLOCKS; ++i) {
		if (topBlocks[i].at && (char*) topBlocks[i].at < low) low = (char*) topBlocks[i].at;
	}
	fake_heap_end = low;
}

static uint8_t* top_alloc(size_t size) {
	size = (size + 0xFFF) & ~(size_t) 0xFFF;
	__malloc_lock(_REENT);
	if (!topEnd) topEnd = fake_heap_end;
	char* brk = sbrk(0);
	int slot = -1;
	for (int i = 0; i < TOP_BLOCKS && slot < 0; ++i) {
		if (!topBlocks[i].at) slot = i;
	}
	uint8_t* p = NULL;
	/* the highest gap that takes it: between the blocks, else below the lowest */
	for (char* ceiling = topEnd; slot >= 0;) {
		int below = -1;
		for (int i = 0; i < TOP_BLOCKS; ++i) {
			if (topBlocks[i].at && (char*) topBlocks[i].at < ceiling && (below < 0 || topBlocks[i].at > topBlocks[below].at)) below = i;
		}
		char* floor = below >= 0 ? (char*) topBlocks[below].at + topBlocks[below].size : brk + HEAP_ROOM;
		if (ceiling - floor >= (ptrdiff_t) size) {
			p = (uint8_t*) (ceiling - size);
			break;
		}
		if (below < 0) break;
		ceiling = (char*) topBlocks[below].at;
	}
	if (p) {
		topBlocks[slot].at = p;
		topBlocks[slot].size = size;
		hostTopBytes += size;
		top_end_update();
	}
	__malloc_unlock(_REENT);
	return p;
}

static void top_free(uint8_t* p) {
	__malloc_lock(_REENT);
	for (int i = 0; i < TOP_BLOCKS; ++i) {
		if (topBlocks[i].at != p) continue;
		hostTopBytes -= topBlocks[i].size;
		topBlocks[i].at = NULL;
		top_end_update();
	}
	__malloc_unlock(_REENT);
}

static void rom_free(uint8_t* p, bool top) {
	if (top) top_free(p);
	else free(p);
}

static uint8_t* read_rom(const char* path, size_t* n, bool* top) {
	*top = false;
	if (emu_is_archive_name(path)) return emu_rom_read_all(path, n);
	struct stat st;
	*n = 0;
	if (stat(path, &st) || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return NULL;
	FILE* f = fopen(path, "rb");
	uint8_t* p = f ? top_alloc((size_t) st.st_size) : NULL;
	if (p) *top = true;
	else if (f) p = malloc((size_t) st.st_size);
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
	if (p) rom_free(p, *top);
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
		rom_free(roms[slot].rom, roms[slot].top);
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
		bool top;
		uint8_t* rom = read_rom(path, &size, &top);
		pthread_mutex_lock(&lock);
		if (!rom) {
			pthread_mutex_unlock(&lock);
			return false;
		}
		slot = find_rom(path);
		if (slot >= 0) {
			rom_free(rom, top); /* (read twice at once: the first one kept) */
		} else {
			for (int i = 0; i < MAX_ROMS && slot < 0; ++i) {
				if (!roms[i].rom) slot = i;
			}
			if (slot >= 0) {
				snprintf(roms[slot].path, sizeof(roms[slot].path), "%s", path);
				roms[slot].rom = rom;
				roms[slot].size = size;
				roms[slot].top = top;
				roms[slot].users = 0;
			} else {
				rom_free(rom, top);
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
			rom_free(r->rom, r->top);
			r->rom = NULL;
			r->users = 0;
		}
	}
	pthread_mutex_unlock(&lock);
}
