# FPS Screen Magnifier

A Windows-only, GPU-rendered screen magnifier intended for games running in
Borderless Windowed mode.

## Run

Install Python 3 and the required packages:

```powershell
py -m pip install customtkinter keyboard mss numpy pyopengl pywin32
py main.py
```

The GUI detects connected monitors and lets you choose which display to
magnify. Use **Refresh** after connecting, disconnecting, or rearranging a
display. The magnifier stays centered on the selected monitor, including
monitors positioned above or to the left of the primary display.

The GUI also controls zoom, capture size, filtering, frame rate, and global
hotkeys. Default keybinds are defined in `Config.py` and can be rebound while
the app is running.

## Performance

The default 45 FPS and linear filter provide a balance between smoothness and
game performance. If a game stutters, select 30 FPS and keep the capture region
as small as practical. The renderer automatically avoids capturing and drawing
parts of an enlarged overlay that would be outside the selected monitor.

## Build

Install PyInstaller and run the provided batch file:

```powershell
py -m pip install pyinstaller
build.bat
```

The executable is written to `dist\FPSMagnifier.exe`.
