#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <vector>
#include <algorithm>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shcore.lib")

// ---------- App metadata ----------
static const wchar_t* kAppName   = L"MouseAligner";
static const wchar_t* kClassName = L"MouseAlignerTrayWindow";
static const UINT WM_TRAYICON    = WM_APP + 1;
static const UINT TRAY_UID       = 1;

// Tray menu commands
enum TrayCmd : UINT {
    CMD_TOGGLE_ENABLE = 1001,
    CMD_RELOAD        = 1002,
    CMD_EXIT          = 1003
};

struct MonitorInfo {
    RECT phys;        // physical px in virtual desktop space
    double scale;     // dpi/96
    double dipTop;    // phys.top / scale
    double dipHeight; // phys.height / scale
    std::wstring name;
};

enum class Mode { Top, Center };

// ---------- Globals ----------
static std::vector<MonitorInfo> g_monitors;
static MonitorInfo g_left{}, g_right{};
static int g_boundaryX = 0;

static HHOOK g_hook = nullptr;
static bool g_warpInProgress = false;
static bool g_haveLast = false;
static POINT g_lastPt{};

static bool g_debug = false;
static bool g_console = false;
static bool g_listOnly = false;
static bool g_useTray = true;
static bool g_enabled = true;

static int g_leftIndex = -1;
static int g_rightIndex = -1;
static double g_leftScaleOverride = 0.0;
static double g_rightScaleOverride = 0.0;
static Mode g_mode = Mode::Top;

static NOTIFYICONDATAW g_nid{};
static HWND g_hwnd = nullptr;

// ---------- Utils ----------
static double Clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static double ParseDouble(const char* s) { return std::strtod(s, nullptr); }
static int ParseInt(const char* s) { return std::strtol(s, nullptr, 10); }

static void EnsureConsole() {
    if (g_console || g_debug) {
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleTitleW(kAppName);
    }
}

static double GetScaleForMonitor(HMONITOR hm) {
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return dpiX / 96.0;
    return 1.0;
}

// ---------- Tray ----------
static void UpdateTrayTooltip() {
    if (!g_useTray) return;
    wchar_t tip[128];
    swprintf_s(tip, L"%s (%s)", kAppName, g_enabled ? L"Enabled" : L"Disabled");
    wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon(HWND hwnd) {
    if (!g_useTray) return;

    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, kAppName, _TRUNCATE);

    Shell_NotifyIconW(NIM_ADD, &g_nid);
    UpdateTrayTooltip();
}

static void RemoveTrayIcon() {
    if (!g_useTray) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_TOGGLE_ENABLE, g_enabled ? L"Disable" : L"Enable");
    AppendMenuW(menu, MF_STRING, CMD_RELOAD, L"Reload monitors");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit");

    POINT p; GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ---------- Monitor enumeration ----------
static BOOL CALLBACK EnumMonProc(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;

    RECT phys = mi.rcMonitor;
    double scale = GetScaleForMonitor(hm);

    double physHeight = double(phys.bottom - phys.top);
    double dipTop = phys.top / scale;
    double dipHeight = physHeight / scale;

    g_monitors.push_back(MonitorInfo{ phys, scale, dipTop, dipHeight, mi.szDevice });
    return TRUE;
}

static void EnumerateMonitors() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, 0);

    std::sort(g_monitors.begin(), g_monitors.end(),
        [](const MonitorInfo& a, const MonitorInfo& b) {
            return a.phys.left < b.phys.left;
        });
}

static void PrintMonitors() {
    std::printf("Monitors (sorted left->right):\n");
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        auto& m = g_monitors[i];
        auto& b = m.phys;
        std::printf("  [%zu] %ls  phys=[%ld,%ld - %ld,%ld]  scale=%.3f  dipH=%.1f\n",
            i, m.name.c_str(), b.left, b.top, b.right, b.bottom, m.scale, m.dipHeight);
    }
}

static void ApplyOverrides() {
    if (g_leftScaleOverride > 0.0) {
        g_left.scale = g_leftScaleOverride;
        double physHeight = double(g_left.phys.bottom - g_left.phys.top);
        g_left.dipTop = g_left.phys.top / g_left.scale;
        g_left.dipHeight = physHeight / g_left.scale;
    }
    if (g_rightScaleOverride > 0.0) {
        g_right.scale = g_rightScaleOverride;
        double physHeight = double(g_right.phys.bottom - g_right.phys.top);
        g_right.dipTop = g_right.phys.top / g_right.scale;
        g_right.dipHeight = physHeight / g_right.scale;
    }
}

static bool SelectMonitors() {
    if (g_monitors.size() < 2) return false;

    int li = (g_leftIndex >= 0) ? g_leftIndex : 0;
    int ri = (g_rightIndex >= 0) ? g_rightIndex : 1;

    if (li < 0 || ri < 0 || li >= (int)g_monitors.size() || ri >= (int)g_monitors.size() || li == ri)
        return false;

    g_left = g_monitors[li];
    g_right = g_monitors[ri];

    if (g_left.phys.left > g_right.phys.left)
        std::swap(g_left, g_right);

    ApplyOverrides();
    g_boundaryX = g_right.phys.left;
    return true;
}

// ---------- Warp logic ----------
static void WarpDipAligned(bool leftToRight, int srcYPhys) {
    const MonitorInfo& from = leftToRight ? g_left : g_right;
    const MonitorInfo& to   = leftToRight ? g_right : g_left;

    double srcYDip = srcYPhys / from.scale;

    double relDip = 0.0;
    double newYDip = 0.0;

    if (g_mode == Mode::Top) {
        relDip = (srcYDip - from.dipTop) / from.dipHeight;
        relDip = Clamp(relDip, 0.0, 1.0);
        newYDip = to.dipTop + relDip * to.dipHeight;
    } else {
        double fromCenter = from.dipTop + from.dipHeight * 0.5;
        double toCenter   = to.dipTop + to.dipHeight * 0.5;
        double relCenter  = (srcYDip - fromCenter) / from.dipHeight;
        newYDip = toCenter + relCenter * to.dipHeight;

        relDip = (newYDip - to.dipTop) / to.dipHeight;
        relDip = Clamp(relDip, 0.0, 1.0);
    }

    int newYPhys = (int)std::llround(newYDip * to.scale);

    if (newYPhys < to.phys.top) newYPhys = to.phys.top;
    if (newYPhys >= to.phys.bottom) newYPhys = to.phys.bottom - 1;

    int newXPhys = leftToRight ? to.phys.left + 2 : to.phys.right - 2;

    if (g_debug) {
        std::printf("  srcYPhys=%d srcYDip=%.1f relDip=%.4f -> newYDip=%.1f newYPhys=%d\n",
            srcYPhys, srcYDip, relDip, newYDip, newYPhys);
    }

    g_warpInProgress = true;
    SetCursorPos(newXPhys, newYPhys);

    g_lastPt = { newXPhys, newYPhys };
    g_haveLast = true;
}

// ---------- Low-level mouse hook ----------
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_MOUSEMOVE) {
        if (g_warpInProgress) {
            g_warpInProgress = false;
            return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }

        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        int x = ms->pt.x, y = ms->pt.y;

        if (g_haveLast) {
            if (g_lastPt.x < g_boundaryX && x >= g_boundaryX) {
                if (g_debug) std::printf("Cross L->R (%ld,%ld)->(%d,%d)\n", g_lastPt.x, g_lastPt.y, x, y);
                if (g_enabled) {
                    WarpDipAligned(true, g_lastPt.y);
                    return 1;
                }
            }
            if (g_lastPt.x >= g_boundaryX && x < g_boundaryX) {
                if (g_debug) std::printf("Cross R->L (%ld,%ld)->(%d,%d)\n", g_lastPt.x, g_lastPt.y, x, y);
                if (g_enabled) {
                    WarpDipAligned(false, g_lastPt.y);
                    return 1;
                }
            }
        }

        g_lastPt = { x, y };
        g_haveLast = true;
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ---------- Window proc ----------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            AddTrayIcon(hwnd);
            return 0;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu(hwnd);
            } else if (lParam == WM_LBUTTONUP) {
                g_enabled = !g_enabled;
                UpdateTrayTooltip();
                if (g_debug) std::printf("Enabled=%d\n", g_enabled ? 1 : 0);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case CMD_TOGGLE_ENABLE:
                    g_enabled = !g_enabled;
                    UpdateTrayTooltip();
                    if (g_debug) std::printf("Enabled=%d\n", g_enabled ? 1 : 0);
                    break;

                case CMD_RELOAD:
                    EnumerateMonitors();
                    if (!SelectMonitors()) {
                        if (g_debug) std::printf("Reload failed: invalid monitor selection\n");
                    } else {
                        if (g_debug) std::printf("Monitors reloaded. BoundaryX=%d\n", g_boundaryX);
                    }
                    UpdateTrayTooltip();
                    break;

                case CMD_EXIT:
                    PostQuitMessage(0);
                    break;
            }
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------- Args ----------
static void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "--debug") g_debug = true;
        else if (a == "--console") g_console = true;
        else if (a == "--no-tray") g_useTray = false;
        else if (a == "--list") g_listOnly = true;
        else if (a == "--left" && i + 1 < argc) g_leftIndex = ParseInt(argv[++i]);
        else if (a == "--right" && i + 1 < argc) g_rightIndex = ParseInt(argv[++i]);
        else if (a == "--left-scale" && i + 1 < argc) g_leftScaleOverride = ParseDouble(argv[++i]);
        else if (a == "--right-scale" && i + 1 < argc) g_rightScaleOverride = ParseDouble(argv[++i]);
        else if (a == "--mode" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "top") g_mode = Mode::Top;
            else if (m == "center") g_mode = Mode::Center;
            else {
                EnsureConsole();
                std::puts("Invalid --mode. Use top|center.");
                std::exit(1);
            }
        } else {
            EnsureConsole();
            std::puts("Unknown arg.");
            std::printf("Usage:\n");
            std::printf("  MouseAligner.exe [--list] [--left N --right M]\n");
            std::printf("                   [--left-scale S --right-scale S]\n");
            std::printf("                   [--mode top|center] [--debug] [--console] [--no-tray]\n");
            std::exit(1);
        }
    }
}

// ---------- Entry point (GUI subsystem) ----------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    auto argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> argvA;
    argvA.reserve(argc);

    // convert wide argv to utf8-ish narrow for simple parsing
    for (int i = 0; i < argc; ++i) {
        char buf[512]{};
        WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, buf, (int)sizeof(buf), nullptr, nullptr);
        argvA.emplace_back(buf);
    }
    LocalFree(argvW);

    std::vector<char*> argvPtrs;
    argvPtrs.reserve(argvA.size());
    for (auto& s : argvA) argvPtrs.push_back(s.data());

    ParseArgs((int)argvPtrs.size(), argvPtrs.data());

    // DPI aware so bounds are physical px
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    EnsureConsole();

    EnumerateMonitors();
    if (g_listOnly) {
        PrintMonitors();
        return 0;
    }

    if (!SelectMonitors()) {
        if (!g_debug && !g_console) AllocConsole(); // show error at least once
        std::puts("Failed to select monitors. Use --list to see options.");
        return 1;
    }

    if (g_debug) {
        std::printf("%ls starting. mode=%s boundaryX=%d\n",
            kAppName, g_mode == Mode::Top ? "top" : "center", g_boundaryX);
    }

    // Create hidden window (message-only). Tray icon hooks into this.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(
        0, kClassName, L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    // Hook mouse globally
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        if (!g_debug && !g_console) AllocConsole();
        std::printf("SetWindowsHookEx failed: %lu\n", GetLastError());
        return 1;
    }

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    return 0;
}
