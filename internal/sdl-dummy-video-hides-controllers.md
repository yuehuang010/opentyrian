---
description: "SDL_VIDEODRIVER=dummy reports zero game controllers on macOS — headless runs can't test controller code; use the real video driver"
---

# `SDL_VIDEODRIVER=dummy` hides controllers on macOS

The headless recipe in `CLAUDE.md` (`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
./opentyrian --data ./tyrian21`) **silently reports `no controllers detected`** even
with a controller connected and working. Same binary, same flags, only dropping
`SDL_VIDEODRIVER=dummy`:

```
$ SDL_VIDEODRIVER=dummy ./opentyrian --data ./tyrian21   ->  no controllers detected
$ ./opentyrian --data ./tyrian21                          ->  controller detected: Xbox One S Controller
```

**Why:** macOS routes modern pads (Xbox, PS, MFi) through **GameController.framework**,
whose device discovery is delivered as `GCController` connect notifications on the
**Cocoa run loop**. The dummy video driver never creates an `NSApplication`/run loop, so
those notifications never fire and SDL enumerates zero controllers. It is not a
permissions issue and not a bug in our code — the pad is fine.

Confirmed the pad was genuinely present while SDL saw nothing:
`system_profiler SPBluetoothDataType` listed it under **Connected:**, and
`ioreg -c IOHIDDevice -r -l` showed both `"Product" = "Xbox Wireless Controller"` and a
GameController.framework synthetic device (`"_GCSyntheticDeviceType" = "Xbox360Controller"`).

**How to apply:**
- **Never conclude "controllers are broken" from a `SDL_VIDEODRIVER=dummy` run.** A
  headless run only validates the `controllers == 0` paths — which is still worth doing,
  since that's the main regression risk when reworking input.
- To test controller code **with** hardware, run with the real video driver
  (`SDL_AUDIODRIVER=dummy ./opentyrian --data ./tyrian21` is enough to keep audio quiet).
- To test controller code **without** hardware, use SDL's virtual gamepad —
  `SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
  SDL_CONTROLLER_BUTTON_MAX, 0)`. It works headlessly (it needs no run loop) and SDL
  treats it as a real game controller. `src/controller.c` links standalone against
  `config_file.c` plus ~7 stubs; see the verification section of
  [plan/CONTROLLER_PLAN.md](plan/CONTROLLER_PLAN.md).

Related: [data-dir-flag-is-t-not-d.md](data-dir-flag-is-t-not-d.md) — the other run-flag
trap for headless runs.
