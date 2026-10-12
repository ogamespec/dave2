# Dangerous Dave 2 port -- architecture

The port has exactly two layers.  The rule is simple: **the game layer must not
know that SDL exists**, and the platform layer must not contain game logic.

```
port/src/
├── game layer -- restored DOS procedures, one C function per procedure,
│                each annotated with its original address; no SDL.
│   ├── objects.c   object system: 0x1D76 update, per-frame AIs, reactions
│   ├── dave.c      Dave's states: 0x3891/0x3B6F/0x3DE0/0x3C4C/0x43A9 ...
│   ├── level.c     level loading, spawn tags, tile scans (0x0DAD, 0x4422)
│   ├── video.c     original video routines: PIC, panels 0x7FC9, tiles 0x915A
│   ├── sound.c     PC-speaker engine: 0x8CB2 queue, 0x8DA2 tick, 0x8CE8 stop
│   ├── input.c     input model DS:0x8BB2: 0x71A5, 0x86AC, 0x41C (DOS codes)
│   └── dd2.c       asset decoders: HUFF, RLEW, RLE, EGA planes, PIC
│
├── platform layer -- the only code that talks to the host (SDL3).
│   ├── platform.h  the bridge interface
│   └── platform.c  window, renderer, texture, audio device, keyboard/gamepad,
│                   timers, file dumps, logging
│
├── main.c          glue: argument parsing, the frame loop (0x1EAB order),
│                   screens and the F-menu dispatch
└── dd2_*.h         generated data tables (sprites, actor info, tile
                    attributes, fonts, sound data) built from the EXE
```

## The bridge

| platform.h | purpose |
|---|---|
| `Plat_VideoInit` / `Plat_VideoQuit` / `Plat_Quit` | window, renderer, texture |
| `Plat_Present(idx, w, h, pal)` | show one indexed frame with a palette |
| `Plat_Delay` | frame pacing (also the original's 0x8EC3 retrace wait) |
| `Plat_AudioInit` / `Plat_AudioQuit` / `Plat_AudioTone` | square-wave output |
| `Plat_PumpEvents` / `Plat_Keys` / `Plat_QuitRequested` | input state |
| `Plat_WaitScancode` / `Plat_ScancodeChar` / `Plat_ScancodeName` | key waits |
| `Plat_PadCmd` / `Plat_PadButtons` | gamepad (original 0x73AE / 0x7386) |
| `Plat_Ticks` / `Plat_BasePath` / `write_bmp` / `Plat_ReadPixelsBmp` | misc |
| `Plat_Log` / `Plat_MessageBox` | diagnostics (ASCII only -- the Windows console is not UTF-8) |

## Conventions

* Keys travel as **DOS scan codes**, the original's INT 9 codes.  The platform
  keeps a translated 128-entry state array; SDL key codes never leave
  `platform.c`.
* Sound travels as a **PIT divisor**: the engine sets it, the platform turns it
  into a square wave at `1193182 / divisor` Hz.  `0` means silence.
* Video travels as an **indexed framebuffer plus a 16-entry palette**.  The game
  layer owns the EGA plane emulation (`Video_ToIndexed`) and the platform only
  blits the result.
* Every game function carries the address of the original procedure it was
  restored from, so the code can be checked against `re/ref/orig` and the
  coverage map in `port/COVERAGE.md`.
