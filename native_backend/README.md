# Native magnifier backend prototypes

## Recommended: Windows Graphics Capture magnifier

`WgcMagnifierPrototype.exe` captures the selected monitor with Windows Graphics
Capture, keeps the pixels on the GPU, copies only the centered crop, and renders
it into a Direct3D 11 overlay. It excludes its own overlay from capture to avoid
feedback. Because the monitor is the source, moving or closing an application
does not break capture, and the empty desktop can be magnified too.

Build the prototypes with `build.bat`, then launch the WGC version:

```powershell
build\WgcMagnifierPrototype.exe --list-monitors
build\WgcMagnifierPrototype.exe --monitor 1 --radius 200 --zoom 2.0
```

The overlay appears in the center of the selected monitor. `F8` toggles it and
`F9` exits. Use `--duration 10` for a bounded test.

From WSL:

```bash
./build/WgcMagnifierPrototype.exe --list-monitors
./build/WgcMagnifierPrototype.exe --monitor 1 --radius 200 --zoom 2.0
```

On exit, the prototype prints the number and average rate of frames received
from Windows so a stale capture source can be distinguished from a rendering
problem.

## DWM thumbnail experiment

`DwmMagnifierPrototype.exe` is retained for comparison. It creates a live DWM
thumbnail without running a capture/render loop, but some games stop supplying
fresh content to the thumbnail after startup. Use the WGC version for game
testing.

## Desktop Duplication experiment

This is an isolated proof of concept for replacing the Python/MSS render path.
It captures a selected Windows monitor with Desktop Duplication, copies only the
center region into another Direct3D 11 texture, and enlarges that texture into a
topmost Direct3D overlay. Captured pixels remain on the GPU; the prototype does
not map a texture into CPU memory or upload a frame from Python.

It does **not** replace or connect to the existing Python application yet. Its
purpose is to compare smoothness and game impact before committing to a full
backend rewrite.

The centered Desktop Duplication experiment is retained as
`GpuMagnifierPrototype.exe`, but is not currently recommended: excluding an
overlay that covers its own monitor-capture source can produce a black source
rectangle on this system. The DWM prototype avoids that circular dependency by
capturing a specific application window, while the recommended WGC prototype
uses Windows' overlay-exclusion support with a monitor capture.

## Requirements

- Windows 10 or 11
- Visual Studio 2022 or Visual Studio 2022 Build Tools
- The **Desktop development with C++** workload and a Windows SDK
- A game running in Borderless Windowed mode

## Build

Open PowerShell or Command Prompt in this directory and run:

```powershell
build.bat
```

The executables are written to `build\WgcMagnifierPrototype.exe`,
`build\DwmMagnifierPrototype.exe`, and `build\GpuMagnifierPrototype.exe`.

The build can also be started from WSL, while still using the Windows compiler:

```bash
cd /mnt/c/Users/Qi/vscode/project1/ScreenMagnifier/native_backend
cmd.exe /d /c "call build.bat"
```

## Desktop Duplication experiment options

First list the monitors in DXGI enumeration order:

```powershell
build\GpuMagnifierPrototype.exe --list-monitors
```

Then launch the overlay. For example:

```powershell
build\GpuMagnifierPrototype.exe --monitor 1 --radius 200 --zoom 2.0
```

From WSL, use:

```bash
./build/GpuMagnifierPrototype.exe --monitor 1 --radius 200 --zoom 2.0
```

- `F8` toggles the overlay.
- `F9` exits the prototype.
- `--radius` is half the source region width, matching the Python app's current
  configuration model.
- `--present vsync` is the default. The corrected loop waits for an open
  compositor slot before capture, then synchronizes presentation with the
  selected display.
- `--present immediate` submits without the VSync synchronization and remains
  available for an A/B comparison. It can do unnecessary work and was less
  consistent in the local test.
- `--stats` prints capture rate, presentation rate, the largest presentation
  and source gaps, and relevant Desktop Duplication event counts. It prints
  once at shutdown; logging is disabled by default.
- `--duration 10` is optional and automatically exits after ten seconds. It is
  useful for a quick smoke test; normal interactive runs have no time limit.

## Prototype limitations

- The selected monitor must use landscape orientation. Rotation handling is
  intentionally deferred until the GPU path has proven worthwhile.
- Monitor selection, zoom, and radius are command-line options only.
- There is no settings persistence or Python GUI integration yet.
- The WGC crop and overlay are both centered automatically in the selected
  monitor, regardless of application-window position.
- The WGC overlay uses `WDA_EXCLUDEFROMCAPTURE` to prevent recursive capture.
  The prototype stops with an error if Windows cannot apply the exclusion.
- The overlay uses Windows' Desktop Duplication-specific
  `WCA_EXCLUDED_FROM_DDA` compositor attribute to prevent recursive capture.
  The prototype stops with an error if Windows cannot apply the exclusion,
  rather than displaying a protected black rectangle.
- Desktop Duplication can temporarily lose access after display-mode changes,
  lock-screen transitions, or exclusive-fullscreen transitions. This prototype
  exits with an explanation so recovery behavior can be added later.

## What to compare

Run the same game and camera movement with the Python renderer and this
prototype, one at a time. Compare:

- visible judder in the magnified region;
- the game's own frame-time graph or 1% lows;
- GPU utilization/headroom;
- whether the WGC version remains smooth and continues receiving frames during
  fast camera movement.

The result answers whether a full native backend is justified. It is not yet a
production implementation.
