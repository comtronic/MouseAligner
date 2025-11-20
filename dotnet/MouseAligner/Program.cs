using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

static class Program {
    const int WH_MOUSE_LL = 14;
    const int WM_MOUSEMOVE = 0x0200;
    const bool Debug = true;

    static IntPtr _hookId = IntPtr.Zero;
    static LowLevelMouseProc _proc = HookCallback;

    struct MonitorInfo {
        public RECT PhysicalBounds;   // physical px
        public double Scale;          // dpi/96
        public double DipTop;         // PhysicalTop / Scale
        public double DipHeight;      // PhysicalHeight / Scale
    }

    static List<MonitorInfo> _monitors = new();
    static MonitorInfo _left, _right;
    static int _boundaryX;          // in physical px

    static bool _warpInProgress;
    static int _lastX, _lastY;
    static bool _haveLast;

    static void Main() {
        // 1) Make process DPI-aware so we get consistent physical px
        TryEnablePerMonitorDpiAwareness();

        // 2) Enumerate monitors + DPI
        _monitors = EnumerateMonitors();
        if (_monitors.Count < 2) {
            Console.WriteLine("Need at least 2 monitors.");
            return;
        }

        // sort left->right in physical coords
        _monitors.Sort((a,b) => a.PhysicalBounds.Left.CompareTo(b.PhysicalBounds.Left));
        _left = _monitors[0];
        _right = _monitors[1];
        _boundaryX = _right.PhysicalBounds.Left;

        Console.WriteLine("Detected monitors (physical px):");
        foreach (var m in _monitors) {
            var b = m.PhysicalBounds;
            Console.WriteLine($"Monitor: {b.Left},{b.Top} - {b.Right},{b.Bottom}  scale={m.Scale:F3}  dipH={m.DipHeight:F1}");
        }

        Console.WriteLine();
        PrintMonitor("Left ", _left);
        PrintMonitor("Right", _right);
        Console.WriteLine($"Boundary X = {_boundaryX}");
        Console.WriteLine("Running. Ctrl+C to exit.");

        _hookId = SetHook(_proc);

        MSG msg;
        while (GetMessage(out msg, IntPtr.Zero, 0, 0)) {
            TranslateMessage(ref msg);
            DispatchMessage(ref msg);
        }

        UnhookWindowsHookEx(_hookId);
    }

    static void PrintMonitor(string label, MonitorInfo m) {
        var b = m.PhysicalBounds;
        Console.WriteLine($"{label}: {b.Left},{b.Top} - {b.Right},{b.Bottom}  scale={m.Scale:F3} dipTop={m.DipTop:F1} dipH={m.DipHeight:F1}");
    }

    static void TryEnablePerMonitorDpiAwareness() {
        // PER_MONITOR_AWARE_V2 = -4
        var ctx = (IntPtr)(-4);
        SetProcessDpiAwarenessContext(ctx);
    }

    static List<MonitorInfo> EnumerateMonitors() {
        var list = new List<MonitorInfo>();

        MonitorEnumProc callback = (IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data) => {
            var mi = new MONITORINFOEX();
            mi.cbSize = Marshal.SizeOf<MONITORINFOEX>();
            if (!GetMonitorInfo(hMon, ref mi)) return true;

            var phys = mi.rcMonitor;
            var scale = GetScaleForMonitor(hMon);

            var heightPhys = phys.Bottom - phys.Top;
            var dipTop = phys.Top / scale;
            var dipHeight = heightPhys / scale;

            list.Add(new MonitorInfo {
                PhysicalBounds = phys,
                Scale = scale,
                DipTop = dipTop,
                DipHeight = dipHeight
            });

            return true;
        };

        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero);
        return list;
    }

    static double GetScaleForMonitor(IntPtr hMon) {
        // default fallback
        uint dpiX = 96, dpiY = 96;
        if (GetDpiForMonitor(hMon, 0 /*MDT_EFFECTIVE_DPI*/, out dpiX, out dpiY) == 0)
            return dpiX / 96.0;
        return 1.0;
    }

    static IntPtr SetHook(LowLevelMouseProc proc) {
        using var curProcess = Process.GetCurrentProcess();
        using var curModule = curProcess.MainModule;
        var moduleHandle = GetModuleHandle(curModule.ModuleName);
        return SetWindowsHookEx(WH_MOUSE_LL, proc, moduleHandle, 0);
    }

    delegate IntPtr LowLevelMouseProc(int nCode, IntPtr wParam, IntPtr lParam);
    delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, ref RECT lprcMonitor, IntPtr dwData);

    static IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam) {
        if (nCode >= 0 && wParam == (IntPtr)WM_MOUSEMOVE) {
            if (_warpInProgress) {
                _warpInProgress = false;
                return CallNextHookEx(_hookId, nCode, wParam, lParam);
            }

            var data = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
            int x = data.pt.x, y = data.pt.y;

            if (_haveLast) {
                if (_lastX < _boundaryX && x >= _boundaryX) {
                    if (Debug) Console.WriteLine($"Cross L->R from ({_lastX},{_lastY}) to ({x},{y})");
                    WarpDipAligned(true, _lastY);
                    return (IntPtr)1;
                }

                if (_lastX >= _boundaryX && x < _boundaryX) {
                    if (Debug) Console.WriteLine($"Cross R->L from ({_lastX},{_lastY}) to ({x},{y})");
                    WarpDipAligned(false, _lastY);
                    return (IntPtr)1;
                }
            }

            _lastX = x; _lastY = y; _haveLast = true;
        }

        return CallNextHookEx(_hookId, nCode, wParam, lParam);
    }


    static void WarpDipAligned(bool leftToRight, int srcYPhys) {
        var from = leftToRight ? _left : _right;
        var to   = leftToRight ? _right : _left;

        // srcY i DIP
        double srcYDip = srcYPhys / from.Scale;

        // relative DIP-position on source monitor
        double relDip = (srcYDip - from.DipTop) / from.DipHeight;
        relDip = Math.Clamp(relDip, 0.0, 1.0);

        // new DIP-position on target monitor
        double newYDip = to.DipTop + relDip * to.DipHeight;

        // back to physical px
        int newYPhys = (int)Math.Round(newYDip * to.Scale);

        // clamp within physical bounds
        var tb = to.PhysicalBounds;
        if (newYPhys < tb.Top) newYPhys = tb.Top;
        if (newYPhys >= tb.Bottom) newYPhys = tb.Bottom - 1;

        int newXPhys = leftToRight ? tb.Left + 2 : tb.Right - 2;

        if (Debug) {
            Console.WriteLine($"  srcYPhys={srcYPhys} srcYDip={srcYDip:F1} relDip={relDip:F4} -> newYDip={newYDip:F1} newYPhys={newYPhys}");
        }

        _warpInProgress = true;
        SetCursorPos(newXPhys, newYPhys);

        _lastX = newXPhys; _lastY = newYPhys; _haveLast = true;
    }

    // ===== Win32 structs + imports =====

    [StructLayout(LayoutKind.Sequential)]
    struct POINT { public int x, y; }

    [StructLayout(LayoutKind.Sequential)]
    struct MSLLHOOKSTRUCT {
        public POINT pt;
        public int mouseData, flags, time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    struct MSG {
        public IntPtr hwnd;
        public uint message;
        public IntPtr wParam, lParam;
        public uint time;
        public POINT pt;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    struct MONITORINFOEX {
        public int cbSize;
        public RECT rcMonitor, rcWork;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szDevice;
    }

    [DllImport("user32.dll")]
    static extern bool SetProcessDpiAwarenessContext(IntPtr value);

    [DllImport("Shcore.dll")]
    static extern int GetDpiForMonitor(IntPtr hmonitor, int dpiType, out uint dpiX, out uint dpiY);

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll", SetLastError = true)]
    static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr lprcClip, MonitorEnumProc lpfnEnum, IntPtr dwData);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFOEX lpmi);

    [DllImport("user32.dll")]
    static extern bool GetMessage(out MSG lpMsg, IntPtr hWnd, uint wMsgFilterMin, uint wMsgFilterMax);

    [DllImport("user32.dll")]
    static extern bool TranslateMessage(ref MSG lpMsg);

    [DllImport("user32.dll")]
    static extern IntPtr DispatchMessage(ref MSG lpMsg);

    [DllImport("kernel32.dll", CharSet = CharSet.Auto)]
    static extern IntPtr GetModuleHandle(string lpModuleName);
}
