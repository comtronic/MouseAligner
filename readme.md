# MouseAligner

MouseAligner is a small Windows tool that stops your mouse cursor from doing interpretive dance when you move between monitors with different resolutions and DPI scaling.

Typical setup:
- 4K monitor at 125% scaling next to a 1440p monitor at 100% scaling
- Same physical size, different logical pixel heights
- Cursor jumps diagonally or lands at the wrong vertical position in the middle

MouseAligner remaps the cursor so it crosses edges at the same *perceived* height.  
Because apparently that was too much to ask from the OS in 2025.

---

## Why this exists

Windows uses two coordinate spaces when scaling differs:

1. **Physical pixels (device pixels)**  
   - What the low-level mouse hook (`WH_MOUSE_LL`) reports.

2. **DIP / logical pixels (scaled pixels)**  
   - What monitor layouts and UI sizing use.

If you try to align physical mouse coordinates using logical monitor bounds, your cursor will drift or snap vertically. Especially in the middle of the edge, where the mismatch is largest.  
MouseAligner fixes this by doing the math in the *right* space:

- Enable **Per-Monitor DPI Awareness v2**
- Enumerate monitors in physical pixels
- Read per-monitor DPI scale (`GetDpiForMonitor`)
- Convert cursor Y into DIP, map relative height, convert back to physical pixels
- Warp cursor with `SetCursorPos`

Result: your mouse behaves like a sane object in Euclidean space again.

---

## Implementations

Two versions, same core logic:

### 1) C++ Win32 (recommended)
- Source: `cpp/src/MouseAligner.cpp`
- Output: `cpp/build/MouseAligner.exe`

### 2) .NET Console (reference)
- Source: `dotnet/MouseAligner/Program.cs`
- Output: `dotnet/MouseAligner/bin/Release/.../MouseAligner.exe`

---

## Build

### One-command build
Open **x64 Native Tools Command Prompt for VS** (so `cl.exe` is usable), then:

```bat
build.bat
