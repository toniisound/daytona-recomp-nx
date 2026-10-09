# Nintendo Switch target (experimental)

Native libnx/SDL2 frontend for the existing runtime, built as a homebrew
`.nro`. It reuses the Vita frontend's portable pieces (`platform/vita/audio.h`,
`controls.h`, `text.h`, `performance.h`, softfloat config) and the Vita OPT03
CPU renderer; only `main.cpp` and `diagnostic_log.h` are Switch-specific.
The 496x384 software-composited screen is uploaded through SDL2's Switch
renderer. No ROM data or generated game code is included.

## Build (Linux or WSL)

1. devkitPro with the Switch packages:

       sudo dkp-pacman -S switch-dev switch-sdl2

2. Host build with your own ROM set in `roms/daytona93.zip` (this runs the
   importer and recompilers on the PC and generates `build/gen/`):

       python3 scripts/setup.py

3. Cross-build the NRO:

       export DEVKITPRO=/opt/devkitpro
       python3 scripts/build_switch.py            # --icon logo.jpg optional

   Output: `build/switch/daytona_switch.nro`.
   `--compile-check` builds everything except the game code (no ROM needed).

## Install

    sdmc:/switch/daytona93/daytona_switch.nro
    sdmc:/switch/daytona93/daytona93.zip      (complete MAME set, ZIP not 7z)

The game starts straight away; + and − together open the menu (resume,
reset, sound, settings, quit). If the ROM set is missing or wrong, the menu
opens instead and says why.

Launch it from the Homebrew Menu **via title override (hold R while starting
a game)**. Applet/album mode has less memory; the menu warns if you are in it.
Saves (`ioboard_eeprom.bin`, `backup_ram.bin`, and the menu's `mute.bin` and
`overlay.bin`) are written to the same folder. Errors are appended to
`switch-diag.log` there; routine startup and performance logging (every two
seconds, plus `switch.log`) only in a build made with
`build_switch.py --diagnostics`.

The menu's PERFORMANCE BAR shows the frame rate and timings at the bottom
of the game screen (off by default).

## Controls

| Arcade | Switch |
| --- | --- |
| Steer | Left stick / D-pad left-right |
| Gas / brake | ZR / ZL, or right stick up/down (analogue) |
| Shift up / down | R / L, or D-pad up/down |
| View buttons | A B Y X |
| Start / coin | + / − |
| Test / service | L3 / R3 (stick clicks) |
| Menu | + and − together |

## Performance

- The 3D layer and the tilemap layers are drawn on all three application cores: the main thread
  and two workers (cores 1 and 2) each draw every third scanline
  (`Raster::set_parallel`). Each scanline is computed exactly as in the serial
  renderer, so the image is identical (`tests/test_raster_lanes.cpp`,
  `scripts/test_vita_renderer.py`: 2 and 3 lanes against the reference renderer).
- The sound board runs on core 2, one frame behind, overlapping the next
  main-board frame (`GameLoop::run_frame_sound_packet`).
- No vsync: the game's own 57.5 Hz clock paces frames, and only new frames
  are presented. With vsync a frame just over 16.7 ms fell to 30 FPS.
- Menu > CPU CORES switches between 3 cores and 1 core to compare.
