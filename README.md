# Zelda's Adventure: CD-i static recompilation

A native Windows build of *Zelda's Adventure* (Philips CD-i, 1994, PAL/Europe
release). The game's 68000 program is translated ahead of time into C. A
runtime provides the parts of the CD-i the code expects: the CD-RTOS (OS-9)
kernel, the CD file manager, the video decoder (with a Direct3D 11 presenter at
any resolution) and the ADPCM audio processor.

The project is built from the CD-i recompilation template that came out of the
*Link: The Faces of Evil* port. This game needed a good deal more of the
operating system than Link did; see [What this game needed](#what-this-game-needed).

You need your own disc image. None of the game's data is included.

## Building and running

1. Put the disc image in `rom/`: `Zelda's Adventure (Europe).bin` and `.cue`
   (raw 2352-byte sectors).
2. Run `build.bat`. It extracts the program from the disc, recompiles it into
   `gen/`, builds with MSVC and copies two programs to the project root:
   * `zelda.exe`: the original picture (sharp, smooth, CRT and nearest filters);
   * `zelda_hd.exe`: the same game with the Cartoon HD and Cartoon HD+
     upscalers as well (it starts with Cartoon HD).
3. Run either one. They look for the disc image in `rom/` next to the
   program, or you can pass the path to the `.cue` or `.bin`.

Requirements: Windows 10+, Visual Studio 2022 (C compiler), CMake 3.19+ and
Python 3.8+. No third-party libraries are needed.

Saves (the game's NVRAM file), quicksaves, settings and `recomp.log` go to
`saves/`. The two programs share the game's saves and the quicksaves, and keep
separate display settings (`settings.ini`, `settings_hd.ini`).

### Making a release (portable zip and installer)

`package.ps1` builds everything and writes to `dist/`:

| File | Contents |
|------|----------|
| `zelda-<version>-portable.zip` | The game folder: both programs, `game.ini`, `update.ps1`, `README.txt` and an empty `rom/`. Unzip anywhere and run. |
| `zelda-<version>-setup.exe` | Installer (Inno Setup 6). Installs for the current user (no administrator rights), asks for the disc image, and adds Start-menu and optional desktop shortcuts. |

```
.\package.ps1               # build, then package
.\package.ps1 -SkipBuild    # package what is already built
.\package.ps1 -Publish      # also create GitHub release v<version> (gh CLI)
```

The version comes from `"version"` in `game.json`. The programs link the C
runtime statically and need only Windows 10/11 DLLs, so nothing else gets
installed. Silent install: `setup.exe /VERYSILENT /ROM="D:\path\game.cue"`.

**Portable by design.** The programs write only next to themselves: `saves\`
(game saves, quicksaves, settings, log) and `game.ini`. No command-line flag is
needed. So the installer uses a folder the user can write to
(`%LOCALAPPDATA%\Programs\…`), and uninstalling keeps `saves\` and `game.ini`.

**Disc image.** `game.ini` holds `rom=`: a `.cue`, a `.bin` or the folder that
contains them. The installer asks for it. A portable copy asks with a file
dialog the first time the image isn't found, and remembers the answer.
`rom/` next to the program and a path on the command line still work.

**Updates.** On start (not in headless runs), the program asks GitHub for the
latest release of `"update_repo"` in `game.json` (`owner/name`; empty means no
check; `update_repo=` in `game.ini` overrides it). The program offers an update
when the release tag is newer than its version and the release has a
`*-portable.zip` asset (`auto_update=1` in `game.ini` skips the question).
* The program downloads the zip into `update\`, starts `update.ps1` and exits.
* `update.ps1` waits for the game to close, then replaces the program files,
  keeping `saves\`, `rom\` and `game.ini`.
* It logs to `update\update.log`, then starts the new version.

`check_updates=0` in `game.ini`, or `--no-update`, turns the check off.

### Controls

| CD-i | Keyboard | Gamepad |
|------|----------|---------|
| Thumbpad / pointer | Arrow keys (Shift = faster) | Left stick / D-pad |
| Button 1 | Z, Space or A | A |
| Button 2 | X, Ctrl or S | B or Y |
| Both buttons | C or Enter | X or Start |

| Key | Function |
|-----|----------|
| F2 | Quicksave to the current slot |
| F3 | Next quicksave slot (0-9) |
| F4 | Load the current slot |
| F5 | Invincibility on/off |
| F6 | Run-ahead: off, 1 or 2 fields |
| F7 | 60 FPS sprite interpolation on/off |
| F9 | Aspect mode |
| F10 | Filter (`zelda_hd.exe` adds Cartoon HD and Cartoon HD+) |
| F11 or Alt+Enter | Full screen |
| Tab (hold) | Run fast |
| Esc | Quit |

Messages for these keys appear at the top of the picture for two seconds.

## Enhancements

* **Quicksaves.** F2 saves a complete snapshot of the emulated machine to the
  current slot (`saves/quicksave_N.state`), F4 restores it, and F3 steps
  through the ten slots. Slots persist between sessions. A snapshot restores
  exactly: the fields after a load are identical to the fields after the save.
* **Invincibility (F5).** The game takes health off every character in one
  instruction (`sub.w d5,34(a2)` at `0x110E5A`). With invincibility on, the
  runtime skips it when the character being hit is Zelda, so enemies still take
  damage. Switching it on also refills Zelda's hearts. Hits still knock her
  back, and other ways of dying (if the game has any that don't go through
  damage) are not covered.
* **Run-ahead (F6, default 1).** After each field the emulator saves its state,
  runs 1 or 2 more fields with the current input, shows the last of them and
  rolls back. The picture reacts 20 or 40 ms sooner to input. Each extra field
  costs a full field of emulation; with run-ahead 1 and everything else on, the
  game keeps real time.
* **60 FPS sprite interpolation (F7, default on).** The game moves its sprites
  only when it updates the picture, every 3 fields (16.7 times a second on
  PAL). The runtime watches the game's sprite blitters, so for every update it
  knows which sprites were drawn and where, and it knows where the game keeps
  a clean copy of its sprite layer. It shows 60 pictures a second, with every
  moving sprite drawn part of the way between its last two positions, over
  the untouched background. Interpolation shows each update while moving
  towards it, so motion is up to one update (60 ms) behind; run-ahead makes up
  part of that. Menus, cutscenes and anything without sprite updates are shown
  as they are.
* **Cartoon HD and Cartoon HD+ (`zelda_hd.exe`).** Two upscalers for any output
  resolution, designed for this project:
  * **Cartoon HD** first smooths the picture at the game's own resolution with
    an edge-preserving (bilateral) filter, which removes dither noise and keeps
    outlines. It then upscales along the direction of each edge (structure
    tensor), so contours come out smooth and crisp instead of stair-stepped
    or blurred.
  * **Cartoon HD+** adds Kuwahara "cel" flattening of textured areas, dark ink
    outlines around everything drawn on the character layer (Zelda, enemies,
    items, HUD) and a slight saturation boost.

## Status

* Boots, plays the Philips logo and the intro, and reaches the title screen
  and the save-slot menu.
* Creating a save (CREATE, entering a name, Done) and starting it with PLAY
  works: the game starts at the shrine, Zelda can be moved, and walking off a
  screen loads the next one.
* The streamed music, and the sound effects and voice the game writes into the
  same stream, play (see [Sound](#sound)). In the attract demo's fight, 52
  effects are played.
* A recompiled run and a fully interpreted run (`--interp`) of the same
  scripted session give identical frames and audio.
* Headless, the game runs at about 4× real time.

Known gaps:

* Only tested with scripted input so far, not by playing through. The narrator
  is triggered by the game on certain screens; none of the screens reached in
  testing triggered it, so narration itself has not been heard in a test.
* When the save menu comes back after creating a save, the game puts the
  pointer 2 units above the PLAY button's active area, so the first click
  without moving does nothing. Moving the pointer slightly fixes it. The cause
  (a coordinate offset somewhere in the pointer emulation) isn't known yet.
* Interpolation needs the game's sprite updates to be recognised. Sprites drawn
  some other way (if any) are shown without interpolation. Two sprites close
  together can occasionally be matched the wrong way round for one update.
* The 60 FPS pictures are timed by the program, not synchronised to the
  monitor's refresh, so a 60 Hz display can show the occasional uneven frame.
* `SS_SD` functions other than SD_Loop (2) are logged and ignored.
* Full Motion Video (MPEG cartridge) is not supported, and this game doesn't use it.

## Sound

The CD-i audio processor plays one sound map at a time. Zelda's Adventure uses
it as a streaming ring: a map loops (SD_Loop) over a small buffer that the CD
keeps refilling from `zelda_audio.rtf`. To play an effect (an enemy's growl, a
hit) or a voice, the game copies the effect's samples into that ring instead
of the next disc sectors, so they play in place of the music for a moment.
Sound maps are therefore decoded from memory as they play, never in advance.
A looping map hands over to a queued map at the end of its current pass.
Real-time audio routed by the CD (for example the audio channel of a screen
loaded from `over.rtf`) is mixed with the sound maps.

## What this game needed

Compared with the template, Zelda's Adventure uses these CD-i/OS-9 features
(details in `docs/CDI_NOTES.md`):

* **An a6 jump table.** Calls go through `jsr -N(a6)` to `jmp abs.l` stubs in
  the program's static data, the a6 form of an MPW a5 table. The analyzer finds
  the table from the code-relative irefs and resolves the calls statically.
* **Several modules in one file.** `cdi_zelda` also carries `cdi_bpsys`,
  `cdi_vsync` and `cdi_vsync_trap`. The disc gives every file, the program
  included, file number 1, so the extractor tells real-time streams apart by the
  submode RT bit.
* **Concurrent processes.** The game forks `cdi_vsync`, which keeps running
  next to it. The kernel schedules processes by priority, deterministically. A
  process gives up the CPU only at the top of the run loop: a blocking system
  call unwinds the native calls and runs again when the process is resumed. So
  no process state lives on native stacks, which is what makes snapshots
  (quicksaves, run-ahead) possible.
* **OS-9 events** (F$Event): the game's message queue, and the system's
  `line_event`, which is pulsed at every vertical blank.
* **Trap handler modules** (F$TLink, `trap #5`): `cdi_vsync_trap` is the
  per-field display engine.
* **A driver subroutine module.** SetStat $5A installs Philips' `cdi_bpsys`
  (pointer, cursor and hot-spot manager) into the video driver. Its 68k code
  runs in system state against stub driver entry points, including a cyclic
  system-state alarm that polls the pointer.
* **Function pointers in data and an unrolled loop.** The analyzer now also
  follows code pointers in the initialized data, function prologues in gaps,
  and Duff's-device jumps (`jmp T(pc,dN.l)` into an unrolled loop).
* **PAL**: 625 lines, 50 Hz and 280-line fields, selected with `"tv": "pal"`.
* **NVRAM files without open handles.** NVRAM paths keep a host path and a
  position and open the file for each operation, so a snapshot holds no host
  file handles.
* **Sound-map looping** (SS_SD SD_Loop), sound maps re-pointed by the game
  to its own buffers through the descriptor, and sound maps decoded while they
  play (see [Sound](#sound)).

## game.json

```json
{
  "title": "Zelda's Adventure",
  "exe_name": "zelda",
  "tv": "pal",
  "modules": [
    {"file": "auto", "base": "0x100000", "extra_entries": [], "a5_table": "auto"}
  ],
  "invincibility": {"damage_insn": "0x110e5a", "victim_reg": "a2", "globals": "0x140028",
                    "player": "0x139a", "health": "0x22", "max_health": "0x13f2"},
  "sprites": {"draw": ["0x117454", "0x117482", "0x117932", "0x1179a4", "0x117cea", "0x117d44"],
              "restore": ["0x117640"]}
}
```

* **`tv`** is `"pal"` (625 lines, 50 fields/s, 280-line picture) or `"ntsc"`
  (525 lines, 60 fields/s, 240 lines; the default). It sets the CSD the game
  reads, the field rate and the frame size.
* The **first module** is the program the player boots. `"auto"` reads its name
  from the disc label's application identifier (`extracted/_app.txt`). Other
  modules in the same file are registered automatically.
* **Other modules** are programs the game loads at run time with F$Load or
  F$Fork. Each one listed is recompiled for its `base` address, and the runtime
  places it there. Ranges must not overlap and must stay below `0x140000`,
  where the heap starts. Program modules that are *not* listed still run in the
  built-in interpreter. Here `cdi_vsync`, `cdi_vsync_trap` and `cdi_bpsys` are
  small and run that way.
* **`extra_entries`** are code addresses the analyzer can't discover, as
  absolute hex values (`base + offset`). The analyzer already finds:
  * call and branch targets;
  * switch tables;
  * MPW a5 and a6 stub jump tables;
  * Microware `move.l #off,dN / jsr (pc,dN.l)` calls;
  * F$Icpt handlers;
  * `lea`/`pea` function pointers and code pointers in the initialized data
    that start with `link` or `movem`;
  * functions that nothing refers to (a prologue right after the end of a
    function);
  * jumps into unrolled loops.
* **`a5_table`** (`"auto"`) detects MPW-style `jsr N(a5)` tables.
  **`a6_table`** (`"auto"` by default) detects `jsr N(a6)` stub tables.
* **`invincibility`** (optional) enables F5. `damage_insn` is the instruction
  that takes health off any character, and `victim_reg` holds the character
  being hit. The player is the object pointed to by `player` (an offset) in the
  globals block that `globals` points to. `health` (offset in the player) and
  `max_health` (offset in the globals) are words, used to refill on F5.
* **`sprites`** (optional) enables sprite interpolation. It lists the entry
  points of the game's sprite blitters: `draw` routines draw a sprite and
  `restore` routines copy the clean layer back under one. They all take
  `d0` = a descriptor `{run list, destination, clean source}`. The generator
  turns these addresses, and `damage_insn`, into hooks (`game_skip_insn`) in
  both the recompiled code and the interpreter.

To see what the analyzer found, run `python tools/analyze.py [module index]`.
It writes a disassembly to `build/listing_<module>.asm`.

## Layout

| Path | Contents |
|------|----------|
| `tools/cdi_extract.py` | Green Book filesystem reader. Writes program files and `_app.txt` (`--all` also dumps the real-time streams). |
| `tools/m68k.py` | MC68000 decoder |
| `tools/os9mod.py` | OS-9 module header, idata and irefs |
| `tools/analyze.py` | Code discovery (recursive descent plus the patterns above) |
| `tools/gen.py` | 68000→C generator, flag liveness, dispatch tables, `gen/game_config.h` |
| `tools/config.py` | `game.json` loader |
| `tools/gen_tests.py` | Per-instruction differential tests |
| `tools/bmp2png.py` | Converts frame dumps to PNG |
| `runtime/cpu.c` | Dispatcher and run loop |
| `runtime/interp.c` | 68000 interpreter, for code outside the recompiled module and the `--interp` reference mode |
| `runtime/alu.c` | Shifts, division and BCD with exact flags |
| `runtime/os9.c` | OS-9 kernel: memory, processes and scheduler, signals, events, alarms (including system-state ones), trap handler modules, driver subroutine modules, paths, NVRAM, CSD |
| `runtime/state.c` | Snapshots: save/load of the whole emulated machine, quicksave files |
| `runtime/frame.c` | Frame boundary: hotkeys, quicksave slots, cheats, run-ahead, 50/60 Hz presentation |
| `runtime/sprites.c` | Observes the sprite blitters and finds when each game update becomes visible |
| `runtime/motion.c` | 60 FPS sprite interpolation between game updates |
| `runtime/osd.c` | On-screen messages (5×7 font) |
| `runtime/portable_win32.c` | `game.ini`, the first-run disc image dialog, update check and download |
| `package.ps1`, `installer/zelda.iss`, `dist_files/` | Release packaging: portable zip, Inno Setup installer, player README, `game.ini` template, `update.ps1` |
| `runtime/cdfm.c` | CD file manager: reads, and real-time SS_Play with PCB/PCL/CIL |
| `runtime/video.c` | UCM display control (FCT/LCT), MCD212-style renderer, cursor, pointer |
| `runtime/audio.c` | ADPCM levels A/B/C, sound maps with gapless SM_Out and SD_Loop, attenuation |
| `runtime/render_d3d11.c` | GPU presenter: aspect modes, sharp/smooth/CRT/nearest filters; Cartoon HD/HD+ in the HD build (`HD_BUILD`) |
| `runtime/host_win32.c` | Window, input (keyboard and XInput), waveOut, pacing, scripted and fuzz input |
| `test/` | Instruction differential tester and ALU reference vectors |
| `docs/CDI_NOTES.md` | CD-i facts and API codes worked out during the ports |

## Debugging workflow

* **Find what fails.** Unimplemented OS calls and SetStat/GetStat codes are
  logged to `saves/recomp.log`. Fatal errors also show a 68k register dump and a
  stack backtrace.
* **Debug options:**
  * `--trace` / `--trace3` log syscalls (with the process id), signals,
    events and disc activity.
  * `--headless --frames N --dump K` runs fast without a window and writes every
    Kth frame as a BMP to `saves/`.
  * `--script "field:key:dur,..."` scripts input (keys `1 2 L R U D`).
  * `--fuzz SEED` drives the game with random input.
  * `--wav out.wav` records the audio.
  * `--runahead N` and `--smooth 0|1` set run-ahead and 60 FPS interpolation
    (headless runs default to both off, so test runs stay comparable).
  * `--dump-from N` starts frame dumps at field N; `--dump-shows K` writes
    every Kth picture shown (`show_*.bmp`, 60 a second with interpolation).
  * `--load-state FILE` starts from a quicksave; `--ramdump K` writes the RAM
    every K fields; `--capture N FILE` saves the window's picture at field N
    (shows the GPU filters).
  * Script keys `s`, `n`, `l` and `i` press F2, F3, F4 and F5.
  * `--watch ADDR` logs every change of the byte at ADDR (checked once per
    field).
  * `"trace_hooks": ["0x…", …]` in `game.json` logs each execution of those
    addresses, with registers, in `--trace` runs (regenerate with
    `tools/gen.py`). It works for interpreted modules too, such as
    `cdi_bpsys`.
* **Check snapshots and run-ahead.** Save with `s` in a script and load with
  `l`: the fields after the load match the fields after the save. A run with
  `--runahead 1` shows, at field N, exactly field N+1 of a normal run, and
  records identical audio.
* **Check the translation.** Run the same headless session twice, once with
  `--interp`, which executes everything in the interpreter, and compare the
  frame dumps and WAVs. Headless runs are deterministic: emulated time is
  counted in instructions, the clock uses a fixed epoch, and processes switch
  only at system calls and the same yield points in both modes. Any difference
  points to a translation bug. Delete `saves/nvr_zelda.nvri` before each run
  so both start from the same saves.
* **Unit-level checks.** `python tools/gen_tests.py` and
  `python test/gen_alu_vectors.py` generate test sources. Then build the
  `insn_test` and `alu_test` targets:

  ```
  cmake --build build\cmake --config Release --target insn_test alu_test
  build\cmake\Release\insn_test.exe "rom\Zelda's Adventure (Europe).bin" 200
  build\cmake\Release\alu_test.exe
  ```

  The ALU vectors all pass. Three randomized instruction tests differ: they
  make a word access at `0xFFFFFF` (`a2 = -1`), straddling the end of the
  address space. That is an odd address, which a real 68000 would fault on, and
  the game never does it.
* Code that jumps into RAM is not an error: it runs in the interpreter. Jumps
  to *undiscovered* addresses inside a recompiled module are fatal. Add those
  addresses to `extra_entries`, or teach `analyze.py` the pattern.
