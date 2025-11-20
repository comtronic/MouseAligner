# MouseAligner

MouseAligner is a small Windows tool that aligns mouse movement between monitors with different resolutions and DPI scaling. It fixes the classic “cursor jumps diagonally / lands at the wrong height” problem when crossing monitor edges.

Typical setup:
- 4K monitor at 125% scaling next to a 1440p monitor at 100% scaling
- Same physical size, different *logical* pixel heights
- Cursor feels fine near top/bottom but goes drunk in the middle

MouseAligner makes the cursor land at the same **perceived (visual) height** on the other monitor.  
Because apparently that was too much to ask from the OS in 2025.

---

## Why this exists

Windows uses two coordinate spaces when scaling differs:

1. **Physical pixels (device pixels)**  
   - What the low-level mouse hook (`WH_MOUSE_LL`) reports.

2. **DIP / logical pixels (scaled pixels)**  
   - What monitor layouts and UI sizing use.

If you map physical mouse coordinates using logical monitor bounds, your cursor drifts or snaps vertically. Especially around the middle of the edge, where the mismatch is largest.

MouseAligner fixes this by doing the math in the correct space:

- Enable **Per-Monitor DPI Awareness v2**
- Enumerate monitors in physical pixels
- Read per-monitor DPI scale (`GetDpiForMonitor`)
- Convert cursor Y into DIP, map relative height, convert back to physical pixels
- Warp cursor with `SetCursorPos`

Result: your mouse behaves like a sane object again.

---

## Implementations

Two versions in this repo:

### 1) C++ Win32 (recommended)
- Source: `cpp/src/MouseAligner.cpp`
- Output: `cpp/build/MouseAligner.exe`
- Runs **tray-first by default** (no console window unless requested).

### 2) .NET Console (reference)
- Source: `dotnet/MouseAligner/Program.cs`
- Output: `dotnet/MouseAligner/bin/Release/.../MouseAligner.exe`

Both implement the same DPI-correct alignment strategy.

---

## Build

### One-command build
Open **x64 Native Tools Command Prompt for VS** (so `cl.exe` + Windows SDK paths are set), then:

```bat
build.bat
````

Builds:

* `cpp/build/MouseAligner.exe`
* `.NET Release binary under dotnet/.../bin/Release`

### If `cl.exe` says windows.h is missing

You’re not in the native tools prompt. Either:

* use the VS “x64 Native Tools Command Prompt”, or
* run `vcvars64.bat`, or
* build with MinGW/LLVM:

```bat
g++ -std=c++17 cpp/src/MouseAligner.cpp -luser32 -lshcore -lshell32 -o cpp/build/MouseAligner.exe
```

---

## Usage (C++ version)

MouseAligner is **tray-first public software**:

* **Notification area (system tray) icon by default.**
* No console window unless you ask for one.
* Tooltip shows current state: **Enabled / Disabled**.
* **Left-click** tray icon toggles Enabled/Disabled.
* **Right-click** tray icon opens a menu:

  * Enable/Disable
  * Reload monitors
  * Exit

### Run normally (tray icon, quiet)

```bat
MouseAligner.exe
```

Defaults: leftmost monitor = Left, next monitor = Right.

### List monitors

Shows detected monitors and their DPI scales.

```bat
MouseAligner.exe --list --console
```

### Choose monitors explicitly

```bat
MouseAligner.exe --left 0 --right 1
```

### Override DPI scale (if Windows lies)

```bat
MouseAligner.exe --left-scale 1.0 --right-scale 1.25
```

### Mapping mode

* `top` (default): aligns relative to top edge
* `center`: aligns around vertical center (often feels better if panels are physically centered)

```bat
MouseAligner.exe --mode top
MouseAligner.exe --mode center
```

### Debug output

Allocates a console and prints detailed crossing/mapping logs:

```bat
MouseAligner.exe --debug
```

### Force console without debug spam

```bat
MouseAligner.exe --console
```

### Completely headless (no tray icon)

Runs in background with no UI at all:

```bat
MouseAligner.exe --no-tray
```

---

## Autostart

MouseAligner must run as a normal user process (not a Windows service), because services live in Session 0 and can’t hook or control your interactive cursor.

### Startup folder

1. `Win + R`
2. Run:

   ```
   shell:startup
   ```
3. Add a shortcut to `MouseAligner.exe` (optionally with args).

### Task Scheduler (recommended)

1. Task Scheduler → Create Task…
2. Trigger: **At log on**
3. Action: Start `MouseAligner.exe`
4. Optional: “Run with highest privileges”

---

## Notes / Limitations

* Designed for two side-by-side monitors. For more, use `--left/--right`.
* Some fullscreen games use Raw Input and ignore OS cursor warps. MouseAligner can’t fix a game that refuses to listen.
* Complex layouts (vertical stacks, L-shapes) aren’t handled yet.

---

## License

MIT License. Attribution is appreciated.
See `LICENSE`.
