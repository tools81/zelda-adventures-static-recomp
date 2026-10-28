# CD-i / CD-RTOS notes for static recompilation

These are facts worked out while porting *Link: The Faces of Evil* and then
*Zelda's Adventure* (PAL). The Zelda findings are collected in the last section. The Green
Book (CD-i Full Functional Specification, May 1994) describes the APIs but
mostly omits numeric codes. The codes below were inferred from the game's
library wrappers and the parameter lists in the spec. An entry marked *(seen)*
was confirmed by actual use in that game.

## Disc and boot

* Sectors are raw 2352 bytes, Mode 2: sync (12), header (4), subheader
  (file, channel, submode, coding, ×2), then data. Form 1 carries 2048 bytes
  and Form 2 carries 2324 (audio uses 2304).
* The disc label is at LSN 16 (`"CD-I "` at data offset 1). The application
  identifier at label offset 574 names the boot module, for example `cdi_link`.
* Real-time files (`.rtr`, `.rtf`) have file number ≥ 1. Their directory size
  counts only that file's sectors, and padding sectors use file number 0 (on
  Link's disc). Don't rely on the file number to find real-time files:
  Zelda's Adventure gives *every* file, the program included, file number 1,
  and its padding sectors too. The RT bit (0x40) in the first sector's
  submode is reliable.
  Empty sectors (submode with no V/A/D bit) can still carry EOR, trigger or EOF
  and **must be processed** for play-control purposes.

## OS-9 process start (what the runtime emulates)

* The kernel copies idata into static storage and applies irefs. The first
  table is code-relative (+module base) and the second data-relative (+data
  base).
* On entry, **a6 = static storage + 0x8000**, a5 = a7 = parameter area,
  a3 = module, d5 = parameter size and d6 = total memory.
* MPW-built programs (Link): idata begins with a jump table header
  `[size, a5 offset]` followed by `dc.w seg; jmp abs.l` stubs copied to
  `a5+0x20`. `jsr N(a5)` jumps to a stub in RAM, which costs one extra
  instruction. Count it, or cycle-exact comparisons with the interpreter
  drift. Function names are present as MacsBug symbols after each function.
* Microware C programs (the Philips "bumper"): a6-relative data and calls of
  the form `movea.l #off,aN / jsr d8(pc,aN.l)`.

## Kernel behaviour the games rely on

* **Signals** are delivered asynchronously. Games busy-wait on flags that
  their intercept handlers set, so translated code must yield at backward
  branches.
* **F$Sleep and F$Wait clear the signal mask.** The standard pattern is
  "mask, start an operation, sleep".
* While an intercept routine runs, signals are masked. F$RTE restores the
  context and unmasks.
* F$Alarm `A$Cycle` (d1=2) with time bit 31 set means the time is in 1/256 s.
  Link's 16 Hz game tick is `0x80000010`.
* F$Fork + F$Wait: Link's bumper is forked and waited for. Zelda's
  `cdi_vsync` runs alongside the main program for the whole game. The runtime
  schedules processes by priority (aging, 20 ms slices) and switches only in
  system calls and at yield points, so runs stay deterministic. A blocking
  call returns a special pc that unwinds the native calls to the run loop, and
  the process restarts at the `os9` instruction when it runs again. So no
  process state is on a native stack, and the whole machine can be
  snapshotted between fields. Asynchronous signal sources must target the
  process that requested them.
* Common system call numbers: F$Link 00, F$Load 01, F$UnLink 02, F$Fork 03,
  F$Wait 04, F$Exit 06, F$Send 08, F$Icpt 09, F$Sleep 0A, F$ID 0C, F$Time 15,
  F$UnLoad 1D, F$RTE 1E, F$TLink 21, F$SetSys 27, F$SRqMem 28, F$SRtMem 29,
  F$Alarm 56, F$SigMask 57, F$SRqCMem 5C, I$Create 83, I$Open 84, I$Delete 87,
  I$Seek 88, I$Read 89, I$Write 8A, I$ReadLn 8B, I$WritLn 8C, I$GetStt 8D,
  I$SetStt 8E, I$Close 8F.
* Error codes: E$BMode CB, E$MemFul CF, E$UnkSvc D0, E$EOF D3, E$PNNF D8,
  E$MNF DD, E$DevBsy FA.

## Devices and the CSD

* Games read `/nvr/csd`. It holds text Device Status Descriptors of the form
  `type:name:params` ending in CR. Types:
  * 1 is the CD control unit (`/cd`);
  * 2 is the audio processor (`/ap`);
  * 3 is video (`/vid`, with `LI="525"` or `"625"`);
  * 4 is NVRAM (`/nvr`, `SZ#8`);
  * 5 is the pointer (`/ptr`, `CL="d"` for a manoeuvring device).
* GetStat 0x43 on `/nvr` returns free NVRAM bytes in d2 *(seen)*.

## CDFM (`/cd` files)

| SetStat | Code | Notes |
|---------|------|-------|
| SS_Play | 0x2D | a0 = PCB *(seen)* |
| SS_Abort | 0x31 | *(seen)* |
| SS_Seek | 0x30 | |
| SS_Enable / SS_Disable / SS_Cont / SS_Stop / SS_Pause | 0x39 / 0x38 / 0x37 / 0x36 / 0x33 | probable numbering |

* **PCB:** Stat.w, Sig.w, Rec.l, Chan.l, AChan.w, then CIL pointers for video,
  audio and data.
  * PCB_Sig is sent on EOR, trigger and finish.
  * Rec is decremented on each EOR, and the play ends when it reaches 0.
* **PCL:** Ctrl.b, res, SMode.b, Type.b, Sig.w, Nxt.l, Buf.l, BufSz.l (in
  sectors), Err.l, res.w, Cnt.l.
  * When the buffer fills, Ctrl bit 0 is set, the CIL entry advances to Nxt,
    and PCL_Sig is sent.
  * A full PCL that comes round again is an overrun. That ends the play with
    an error.
* Plays run at 75 sectors per second (1×). Audio sectors whose channel is in
  AChan go to the audio processor. Otherwise they go to the audio CIL.

## Audio (`/ap`)

* SS_SM = 0x3B. These are SetStat unless marked GetStat:
  * Creat 0 (coding, groups) returns the id and a0 = data;
  * Out 1 (id, a0 = asynchronous status block);
  * Off 2;
  * Close 3;
  * Info 0 is GetStat and returns a0 = descriptor.
* **SM_Out on a busy processor queues the new soundmap** to start when the
  current sector finishes. Games double-buffer music this way and pace
  themselves on the completion signal (ASY_Sig), so completions must be
  sample-exact.
* SS_SC = 0x3D, SC_Atten d3: bytes 2 and 0 are the direct paths (L→L, R→R),
  bytes 3 and 1 the cross paths. 0xFF is off.
* ADPCM coding byte:
  * bits 0-1 select stereo (01 = stereo);
  * bits 2-3 select the rate (00 = 37.8 kHz, 01 = 18.9 kHz);
  * bits 4-5 select the sample size (00 = 4-bit, 01 = 8-bit).

  A sound group is 128 bytes: 16 parameter bytes (copies at 4..11) followed by
  112 data bytes. The filter coefficients are the XA ones.

## Video (`/vid`)

* SS_DC = 0x56 (d2 = function):

  | Code | Function | Code | Function |
  |------|----------|------|----------|
  | 0 | CrFCT | 0xA | WrLI |
  | 1 | RdFCT | 0xB | DlLCT |
  | 2 | WrFCT | 0xC | FLnk |
  | 3 | RdFI | 0xD | LLnk |
  | 4 | WrFI | 0xE | Exec |
  | 5 | DlFCT | 0x11 | SSig |
  | 6 | CrLCT | 0x12 | Relea |
  | 7 | RdLCT | 0x1B | PRdLCT |
  | 8 | WrLCT | 0x1C | PWrLCT |
  | 9 | RdLI | | |

  Seen: 0, 2, 4, 6, 7, 0xA, 0xC, 0xD, 0xE, 0x11 and 0x1C. 0xF and 0x13 are
  probably Intl and SetCmp; they're ignored.
* LCT line numbers are in UCM coordinates: normal-resolution tables hold half
  the lines. "P" variants use physical lines.
* SS_GC = 0x52: Pos 0, Show 1, Hide 2, Ptn 3, Col 4, Org 5, Blnk 6.
  Coordinates are high resolution (768×480).
* SS_PT = 0x59: Coord 0 (GetStat) returns d0 = buttons (bit 15 = active) and
  d1 = H:V. SSig 1, Relea 2, **Pos 3, Org 4** (note this order).
* Video interrupts come only from DCP instruction `$60`. DC_SSig counts them.
* DCP opcodes:

  | Opcode | Meaning |
  |--------|---------|
  | 10 | NOP |
  | 20 | link (driver-internal) |
  | 40 | display line start |
  | 60 | interrupt |
  | 78 | display parameters (RMS bits 0-1, 10 = run-length) |
  | 80-BF | CLUT entry within the current bank |
  | C0 | coding: CM0 bits 0-3, CM1 bits 8-11, NM bit 19, CS bit 22 |
  | C1 | transparency: TA bits 0-3, TB bits 8-11, bit 23 = no mix |
  | C2 | plane order |
  | C3 | bank |
  | C4 / C6 | transparent colour |
  | C7 / C9 | mask colour |
  | CA / CB | DYUV start values |
  | D0-D7 | mattes |
  | D8 | backdrop |
  | D9 / DA | mosaic |
  | DB / DC | ICF (0-63) |

* RL7: a byte with bit 7 set starts a run of colour b&0x7F whose length is the
  next byte (0 = to the end of the line). Otherwise the byte is a single pixel.
* DYUV deltas: 0,1,4,9,16,27,44,79,128,177,212,229,240,247,252,255.
* Colours are in studio range (black 16, white 235).
* Link draws sprites with **compiled sprite routines loaded from disc into
  RAM**. They run in the interpreter.

## Timing model used by the runtime

* Emulated time counts executed 68k instructions (`CPU_IPS` = 4M per second).
* Video fields (60 Hz NTSC, 50 Hz PAL), CD sectors (75 Hz), alarms and soundmap completions are
  scheduled on that clock. Pacing to the wall clock happens once per field.
* Translated code yields only at backward branches (`BUDGET`), and the
  interpreter yields by the same rule. That keeps `--interp` runs comparable
  frame for frame.

## Zelda's Adventure (Viridis, PAL)

### Program file and code

* `cdi_zelda` holds four modules back to back (each starts where the previous
  one ends, at 2-byte alignment): the program `cdi_zelda`, the subroutine
  module `cdi_bpsys` (type 2), the program `cdi_vsync` and the trap handler
  module `cdi_vsync_trap` (type 0x0B). The kernel registers all of them when
  the file is loaded.
* Calls go through an **a6 stub table**: `jsr -N(a6)` reaches a 6-byte
  `jmp abs.l` in the program's static data (a6 = static + 0x8000). The stub
  addresses are patched by the code-relative irefs. Some stubs point at module
  offset 0, meaning unfilled, and must not be resolved statically.
* Function pointers also live in the initialized data (code-relative irefs
  pointing into code), with no `lea`/`pea` anywhere.
* A few blitters jump into unrolled loops:
  `move.l #N,d3 / sub.l d2,d3 / lsl.l #s,d3 / jmp T(pc,d3.l)`.
* There are no MacsBug symbols.

### Processes and events

* The program forks `cdi_vsync`, which runs **concurrently** for the whole game
  (it never exits). It links the trap handler and loops on trap #5 function 4.
* **F$Event = 0x53**, d1.w = function. Bit 15 of d1 means "activate all
  waiters":

  | Code | Function | Code | Function |
  |------|----------|------|----------|
  | 0 | Ev$Link (a0 name → d0 id) | 6 | Ev$Read (→ d1 value) |
  | 1 | Ev$UnLnk | 7 | Ev$Info (d0 index, a0 buffer) |
  | 2 | Ev$Creat (d0 value, d2 wait inc, d3 signal inc, a0 name) | 8 | Ev$Signl (value += signal inc) |
  | 3 | Ev$Delet (a0 name) | 9 | Ev$Pulse (d2 value, then restore) |
  | 4 | Ev$Wait (d2 min, d3 max → d1 value; value += wait inc on wake-up) | 10 | Ev$Set (d2) |
  | 5 | Ev$WaitR (relative range) | 11 | Ev$SetR |

  The game builds its message queue on an event created with wait increment
  −1 and signal increment +1, so it works as a counting semaphore.
* **`line_event`** is never created by the game: the system provides it.
  Both processes `Ev$Wait` on it with min = max = 1 to wait for the next field.
  The runtime creates it at boot and pulses it with value 1 at every field.
* Pending signals must be delivered on the way out of *every* system call.
  Otherwise a "play finished" signal that arrives during a signal handler stays
  queued while the program blocks in Ev$Wait, and the game hangs.
* An Ev$Wait interrupted by a signal runs the intercept routine and then waits
  again. The runtime resumes at the `os9` instruction itself, so the analyzer
  makes every `os9` instruction an entry point.

### Trap handler modules

* F$TLink (0x21): d0 = trap number, a0 = module name. The handler's entry is
  the module's M$Exec.
* `trap #n` is followed by a function word. The handler is entered with this
  stack: `(sp)` = caller's a6, `4(sp)` = function code (word), `6(sp)` = vector,
  `8(sp)` = return pc. It returns with `movem.l (sp)+,...,a6,a7 / rts`. Loading
  a7 from `(sp)+` keeps the incremented address, which discards the
  function/vector longword.
* `cdi_vsync_trap` functions: 0 set the state block, 2 link `line_event`,
  4 field loop (Ev$Wait, update FCT/LCTs with SS_DC, call the game's callbacks
  with the game's a6), 6 unlink, 8 set a callback.

### Driver subroutine module (`cdi_bpsys`)

* SetStat **0x5A** on `/vid` with a0 = module name installs a subroutine
  module into the driver. The module's entry table (at M$Exec) holds word
  offsets for init, SetStat, GetStat and terminate.
* Afterwards, SetStat codes 0x51/0x52/0x59 with **d2 = 0x4200–0x4212** go to the
  module. It runs in system state with a5 = the caller's register image (R$d0
  at 0, R$d2 at 8, R$a0 at 32) and a2 = driver static storage. It calls back
  into the driver through 70(a2) (SetStat) and 74(a2) (GetStat), with the
  parameters in d1–d7.
  * 4200 / 4202: remember the video / pointer path context.
  * 4204: start a cyclic **system-state alarm**. The F$Alarm call passes a0 = a
    register image (R$sr at offset 64, R$pc at 66). The routine polls the
    pointer (PT_Coord), moves the cursor (GC_Pos) and calls the game's message
    callbacks.
  * 4206 / 4208 / 420C: activate / push / pop a "screen" (FCTs plus pointer
    regions).
  * 420E: set the region list.
  * 4210: copyright string pointer.
  * 4212: version, returned in the high word of d0 (0x1221).
* The runtime runs the module's 68k code in the interpreter with stub driver
  entries, `os9 I$SetStt; rts` and `os9 I$GetStt; rts`.

### Audio

* **SS_SD = 0x3C**, function 2 = **SD_Loop**: d3 = sound map, d4 = first
  group, d5 = last group, d6 = repeat count (the game uses 10000). The map
  plays the range repeatedly, and its completion is signalled only after the
  last repeat.
* The game re-points its sound maps at its own buffers by writing the
  descriptor (from SM_Info): data address at +10, size at +14, group count at
  +26, coding at +30. The audio processor has to read the descriptor at
  SM_Out time.
* Those buffers are the CD play's PCL buffers: `zelda_audio.rtf` (channel 6,
  from sector 1514) streams into a ring of one-sector PCLs, and the sound map
  loops over the same memory. So a sound map must be decoded from RAM as it
  plays. Decoding it once at SM_Out loops the first half second forever.
* An SD_Loop map plays until another map is queued with SM_Out, then finishes
  its current pass and the queued map follows without a gap. The game waits
  for that before queuing the next one.
* Sound effects: `0x1077FE` looks an effect up in a table (globals +6938 /
  +6942), points `globals + 6978` at its samples in RAM and sets a sector count
  (+6982). The stream's per-sector callback (`0x107A5C`) then copies 0x900
  bytes of the effect into the ring in place of the disc data. Voice
  (narration, `0x107DF4`, flag `globals + 6984`) is fed through the same path
  from `zelda_voice.rtf`. `0x107B7C` starts the stream (d1 = 0 ambient,
  1 voice, 2 other).
* Real-time audio routed to the audio processor during a play (PCB AChan, for
  example channel 1 of `over.rtf` screen loads) has to be mixed with the sound
  maps, not only played when no map is active.

### Attract demo, menus and pointer

* Global word `globals + 5012` set means the attract demo is running: the game
  plays recorded input, ignores the player's, and returns to the title after
  840 ticks or on a button press (`0x104170` → `0x104070`). Test sessions
  must go through the menus to reach real play.
* Menus use `cdi_bpsys` hot-spot regions. A region group has an offset (0, 40):
  region y = pointer y − 40. Button presses only reach the game inside a
  region whose event table accepts them (the background region only takes
  "enter").
* In gameplay the game disables the `cdi_bpsys` pointer record (flags 0xA0) and
  reads `PT_Coord` itself. It puts the pointer back at (30, 30) after each read
  and uses the change as the thumbpad direction.

### PAL

* The CSD advertises `LI="625"`, fields run at 50 Hz with 280 lines, and
  pointer/cursor high-resolution coordinates go to 768×560. The game's
  pictures occupy about 240 of the 280 lines.

### Display and sprites (gameplay)

* Plane A is CLUT8 and holds the characters, items and HUD hearts: the sprite
  layer. It is transparent elsewhere. Plane B is a DYUV photograph of the
  current screen, and the two are not mixed (front/back). The sprite layer is
  double-buffered: two 384×280 CLUT8 buffers at about `0x146A40` and
  `0x1B0840` (heap addresses; they depend on allocation order). The game
  flips between them by rewriting the plane A line addresses in the LCT.
* The game updates the picture every 3 fields (16.7 Hz on PAL).
* Sprites are drawn by run-list blitters, which all take d0 = a descriptor
  `{0: run list, 4: destination, 8: second source}`. A run list is a long
  header, then `{skip.w, count.w, count longwords of pixels}` repeated, ended
  by a zero long. `skip` is added to the destination before each run.
  * `0x117454`: copy.
  * `0x117482`: fill with a colour (d4 = 12(a0)).
  * `0x117932`: copy, masked by a priority map (8(a0)) so sprites pass behind
    scenery.
  * `0x1179A4`: copy with AND/OR masks (12/18(a0)).
  * `0x117CEA`: masked plus AND/OR.
  * `0x117D44`: copy (unrolled).
  * `0x117640`: restore the footprint from the clean copy of the layer (8(a0)).
  The clean copy is a third buffer (about `0x174840`) holding the layer
  without moving sprites.
* Player health: globals block = `[0x140028]` (`-32728(a6)`). The player
  object is `[globals + 0x139A]` and its health is the word at `+0x22`: 140 =
  7 hearts, 10 per half heart. The maximum health is the word at
  `globals + 0x13F2`. Every character's damage goes through
  `sub.w d5,34(a2)` at `0x110E5A` (routine `0x110E14`).
