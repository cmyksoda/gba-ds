# GBA DS for the New Nintendo 3DS

This port runs GBA DS (the game on one screen, its own map, HUD or logo on the other) on the 3DS's
two screens: the game on the top screen, the second screen and the touch screen on the bottom one.
It builds on ratherDashing's 3DS port ([ratherDashing/gba-ds](https://github.com/ratherDashing/gba-ds),
branch `3ds`), whose 3DS layer it keeps.

It's an alpha, tested in the Azahar emulator and not yet on a console with this core (see the end).

It is the same app: `src/` (the game list, the menus, the per-game profiles, map captures on a hidden
copy of the game) builds for the 3DS with `#ifdef __3DS__` changes in six of its files, on the same libmgba as
every other system. What differs is underneath:

| | other systems | 3DS |
|---|---|---|
| emulator | libmgba (`src/emu.c`) | ratherDashing's armGBA with its ARM recompiler (`emu/emu_armgba.c`), the default; `CORE=mgba` builds on libmgba instead (`mgba/build-libmgba.sh`) |
| screens, input, sound | SDL2 | `ctr/sdl_ctr.c`: the part of SDL2 the app uses, on the GPU (citro2d), hid and ndsp |
| threads | pthreads | `ctr/host.c`: map captures and logos on the New 3DS's fourth core, the Frame Buffer option's emulation thread on its third |

## Building

No devkitPro needed on the machine: the builds run in the `devkitpro/devkitarm` Docker image.

    make -C 3ds 3ds      # 3ds/ctr/gba-ds.3dsx (the first time, libmgba into deps/mgba-3ds)
    make -C 3ds cia      # 3ds/ctr/gba-ds.cia (makerom and bannertool in ~/3ds-tools/bin)
    make -C 3ds libmgba  # libmgba again, after scripts/mgba-options.sh changes

makerom comes from [3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR/releases) (its Ubuntu
build runs in an `ubuntu:24.04` container), bannertool from
[diasurgical/bannertool](https://github.com/diasurgical/bannertool/releases).

libmgba is the commit and options in `scripts/mgba-options.sh`, built with mGBA's own 3DS toolchain
file and these changes (`mgba/build-libmgba.sh`):

- 32-bit pixels, as on every other system (mGBA's own 3DS port uses 16-bit color).
- No fixed ROM buffer (several copies of a game run at once), and not the start-up code that
  allocates it (`ctru-heap.c`, 32 MiB).
- `mgba/rom-copy-size.patch`: a copy of the game that writes to its ROM (a profile's patch, a
  cartridge's clock or rumble) gets a copy the ROM's size, not 32 MiB.
- `mgba/memory-map-hook.patch`: mGBA's large blocks come from GBA DS's allocator (`ctr/emu_ctr.c`,
  see Memory).
- `mgba/gba-fast-memory.patch`, `gba-ldm-stm.patch`, `gba-dma-batch.patch`, `color-mix.patch`: fewer
  instructions for the same result (short paths for loads, stores and jumps, LDM/STM over the set
  bits, a DMA's units without the event list, a cheaper alpha blend). Pictures, RAM and states are
  bit-identical to unpatched mGBA's.
- `mgba/gba-stall-closed-form.patch`, `gba-jump-inline.patch`, `gba-audio-fast.patch`,
  `gba-audio-silent.patch`: the same kind of change again (the ROM prefetch model's loop as arithmetic,
  the usual jump checked inside the jumping instruction, no memset, division or table copy per sound
  event, the PSG's output made once per run of equal samples). Same pictures, RAM, states
  and sound samples as before.
- `mgba/video-wake.patch`: `video_wake` in `gba-ds.cfg` (0, 1, 3, 7, 15, 31 or 63; 15 as before)
  is how often mGBA's drawing thread is woken, once per N+1 lines.
- `MINIMAL_CORE` taken out of the `flags.h` it writes: the library is built without it, and the
  app's view of mGBA's structs has to match.
- No `.7z` support (`USE_LZMA=OFF`).

The CIA takes armGBA-3DS's `.rsf` (the 80 MB mode, among others), so `make cia` needs
`git submodule update --init third_party/armgba`, as `CORE=armgba` does.

## On the console

- `gba-ds.3dsx` in `sdmc:/3ds/`, or install `gba-ds.cia`. A New 3DS or New 2DS (an Old 3DS isn't
  tested).
- Games (`.gba`, or one in a `.zip`) in `sdmc:/roms/gba/`. Another folder: `rom_dir` in
  `sdmc:/3ds/gba-ds/gba-ds.cfg`, which is written with the 3DS's defaults the first time the app runs.
- Only games GBA DS has second-screen support for are listed, as everywhere; `show_all = 1` in
  `gba-ds.cfg` lists every game.
- The HUD pictures on the bottom screen (Mario Kart, Crash Nitro Kart, Sonic Advance, V-Rally 3 and the
  like) are a backdrop with the HUD's pieces laid over it by the GPU, so a frame sends only the rows
  that changed. `hud_layers = 0` in `gba-ds.cfg` makes the whole picture each frame instead.
- Sound needs the DSP firmware dump (`sdmc:/3ds/dspfirm.cdc`), as all 3DS homebrew does.
- Saves are beside the ROMs (`.sav`), save states and settings in `sdmc:/3ds/gba-ds/states/`, the log
  in `sdmc:/3ds/gba-ds/log.txt` (the run before it: `log-prev.txt`). Save states are mGBA's, as on
  every other system: states made with the desktop build load on the 3DS (the other way hasn't been
  tried).

Buttons: A, B, L, R, START, SELECT and the D-pad (or the Circle Pad) are the GBA's. **ZL** is GBA DS's
menu button (its HOME: the pause menu, and held with another button, the hotkeys: ZL + R saves a
state, ZL + L loads it, ZL + X the quick menu, ZL + Y swaps the screens, ZL + B changes the shader,
ZL + START closes the game). **ZR** is fast-forward, without sound (`ff_mute = 0` in `gba-ds.cfg`
plays it sped up). The menus also work by touch.

The first time a folder of games is read, each file is opened once for its game code ("Reading
games" on the bottom screen: a few seconds per hundred files); the codes are kept in
`states/archives.cache`, and later starts list the folder at once. Each supported game's logo is
made the first time, in the background, by running its title screen: the list is slower until those
are done.

## Logs over Wi-Fi

A diagnosis build (never a release) POSTs its logs to a machine on the local network, so the SD
card stays in.

    python3 3ds/tools/logserver.py                       # listens on port 8377, writes build/logs/
    touch 3ds/ctr/host.c
    make -C 3ds 3ds LOG_URL=http://<that machine's address>:8377/log

It sends the end of the last run's log (up to 768 KiB) when the app starts, the log so far each time
the game list comes up, and this run's when the app closes. Build without `LOG_URL` again
(after `touch 3ds/ctr/host.c`) for anything given to someone else.

## What isn't there

- Rewind (thirty seconds of states are more than the console's memory), RetroAchievements, the
  updater, `.7z` archives, the bug report's ten seconds of history.
- The game list's notes on romhacks and modified ROMs (the checksum means reading each game whole).
- Map screens that move keep 4 frames, not 16.
- Pokémon Pinball's table over both screens (`pinball_live`): it runs two more copies of the game,
  two frames each, every frame. The game plays on one screen with its logo on the other.
- Screenshots of the screens as drawn (the GPU's picture isn't read back).

## Memory

In the 64 MB mode a `.3dsx` gets, the heap is about 44.6 MiB (45,664 KiB in Azahar, which `host.c`
holds to the console's size), after 12 MiB of linear memory for the GPU and the sound. A game's ROM is read once and shared by every copy of the game (the game, the
hidden copy, a logo's), and kept at the top of the heap's address range with the heap's end brought
down below it, so even a 32 MiB game finds one stretch of memory. mGBA's own large blocks (a ROM copy
for a copy of the game that writes to it) come from there too (`mgba/memory-map-hook.patch`).
MOTHER 3 starts in the 64 MB mode with about 9 MiB of the heap free. The CIA asks for the 80 MB mode.

The Azahar emulator gives 124 MiB; `host.c` keeps the heap to the console's size there, so that what
is tested is what the console has.

## What has and hasn't been tested

In the Azahar emulator (2126.2, a New 3DS whose CPU it runs at 268 MHz, a third of the console's
804), with the heap kept to the console's size:

- Each of these starts without a crash, runs its opening for 30 seconds and shows the logo the hidden
  copy made on the bottom screen: Advance Wars, Advance Wars 2, F-Zero: GP Legend, F-Zero: Maximum Velocity, Fire
  Emblem, Fire Emblem: The Sacred Stones, Golden Sun, Golden Sun: The Lost Age, A Link to the Past,
  Mario Kart: Super Circuit, Mario & Luigi: Superstar Saga, Mario vs. Donkey Kong, Metroid Fusion,
  Metroid: Zero Mission, MOTHER 3 (the English translation 1.1), Pokémon Pinball: Ruby & Sapphire,
  Super Mario Advance 1 to 4 and Wario Land 4 (the USA releases).
- From save states made with the desktop build: Mario Kart's race HUD on the bottom screen, and
  Metroid: Zero Mission's map captured on the hidden copy.
- MOTHER 3 starts in the 64 MB mode. Its in-game captures haven't been tried.

The heaviest scenes logged there take 38 to 44 ms of the emulated CPU's time a frame: Advance Wars'
opening (38.5), F-Zero: Maximum Velocity's (39.6), Fire Emblem's (39.9), Super Mario Advance's
(39.9) and Mario Kart's race with its HUD (43.4). (A map capture ran on the game's core in Azahar
and slowed the game while it lasted; on a New 3DS it has a core of its own.) At the console's clock that's about 13 to 15 ms of the 16.7 a frame has, an estimate
until a console says otherwise.

Not tested: the mGBA core on a console, an Old 3DS, odd-sized ROMs (trimmed dumps, some romhacks),
and the 32 other games GBA DS supports.

On a console, the armGBA core only: ratherDashing's build ran Castlevania: Circle of the Moon at 60
frames a second with its map on a New 3DS (2026-11-12).

## Files

    mgba/              libmgba for the 3DS: build-libmgba.sh and its two patches
    ctr/               the 3DS host: SDL's part (sdl/SDL.h, sdl_ctr.c), host.c, emu_ctr.c (shared ROMs), the CIA's parts
    emu/emu_armgba.c   src/emu.h on armGBA (CORE=armgba), with ctr/jitmem.c
    sim/               the desktop test build of the armGBA core
    test/              the Azahar test's script and SD card files
