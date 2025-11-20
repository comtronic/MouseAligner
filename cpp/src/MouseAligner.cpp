#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellscalingapi.h>
#include <vector>
#include <algorithm>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shcore.lib")

struct MonitorInfo {
    RECT phys;        // physical px i virtual desktop space
    double scale;     // dpi/96
    double dipTop;    // phys.top / scale
    double dipHeight; // phys.height / scale
    std::wstring name;
};

static std::vector<MonitorInfo> g_monitors;

static MonitorInfo g_left{}, g_right{};
static int g_boundaryX = 0;

static HHOOK g_hook = nullptr;
static bool g_warpInProgress = false;
static bool g_haveLast = false;
static POINT g_lastPt{};

static bool g_debug = false;
static bool g_listOnly = false;
static int g_leftIndex = -1;
static int g_rightIndex = -1;
static double g_leftScaleOverride = 0.0;
static double g_rightScaleOverride = 0.0;

enum class Mode { Top, Center };
static Mode g_mode = Mode::Top;

static double Clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double ParseDouble(const char* s) {
    return std::strtod(s, nullptr);
}

static int ParseInt(const char* s) {
    return std::strtol(s, nullptr, 10);
}

static double GetScaleForMonitor(HMONITOR hm) {
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return dpiX / 96.0;
    return 1.0;
}

static BOOL CALLBACK EnumMonProc(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;

    RECT phys = mi.rcMonitor;
    double scale = GetScaleForMonitor(hm);

    double physHeight = double(phys.bottom - phys.top);
    double dipTop = phys.top / scale;
    double dipHeight = physHeight / scale;

    MonitorInfo info;
    info.phys = phys;
    info.scale = scale;
    info.dipTop = dipTop;
    info.dipHeight = dipHeight;
    info.name = mi.szDevice;

    g_monitors.push_back(info);
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

static void SelectMonitors() {
    if (g_monitors.size() < 2) {
        std::puts("Need at least 2 monitors.");
        std::exit(1);
    }

    int li = (g_leftIndex >= 0) ? g_leftIndex : 0;
    int ri = (g_rightIndex >= 0) ? g_rightIndex : 1;

    if (li < 0 || ri < 0 || li >= (int)g_monitors.size() || ri >= (int)g_monitors.size() || li == ri) {
        std::puts("Invalid --left/--right indices.");
        PrintMonitors();
        std::exit(1);
    }

    g_left = g_monitors[li];
    g_right = g_monitors[ri];

    if (g_left.phys.left > g_right.phys.left)
        std::swap(g_left, g_right);

    ApplyOverrides();

    g_boundaryX = g_right.phys.left;
}

static void PrintStartup() {
    auto& L = g_left;
    auto& R = g_right;
    std::printf("MouseFix running.\n");
    std::printf("Left : %ls  phys=[%ld,%ld - %ld,%ld]  scale=%.3f dipH=%.1f\n",
        L.name.c_str(), L.phys.left, L.phys.top, L.phys.right, L.phys.bottom, L.scale, L.dipHeight);
    std::printf("Right: %ls  phys=[%ld,%ld - %ld,%ld]  scale=%.3f dipH=%.1f\n",
        R.name.c_str(), R.phys.left, R.phys.top, R.phys.right, R.phys.bottom, R.scale, R.dipHeight);
    std::printf("BoundaryX=%d  mode=%s  (use --debug for logs)\n",
        g_boundaryX, g_mode == Mode::Top ? "top" : "center");
}

static void WarpDipAligned(bool leftToRight, int srcYPhys) {
    const MonitorInfo& from = leftToRight ? g_left : g_right;
    const MonitorInfo& to   = leftToRight ? g_right : g_left;

    // srcY in DIP
    double srcYDip = srcYPhys / from.scale;

    double relDip = 0.0;
    double newYDip = 0.0;

    if (g_mode == Mode::Top) {
        relDip = (srcYDip - from.dipTop) / from.dipHeight;
        relDip = Clamp(relDip, 0.0, 1.0);
        newYDip = to.dipTop + relDip * to.dipHeight;
    } else { // center mode
        double fromCenterDip = from.dipTop + from.dipHeight * 0.5;
        double toCenterDip   = to.dipTop + to.dipHeight * 0.5;
        double relCenter = (srcYDip - fromCenterDip) / from.dipHeight; // ~[-0.5,0.5]
        newYDip = toCenterDip + relCenter * to.dipHeight;
        relDip = (newYDip - to.dipTop) / to.dipHeight; // for debug
        relDip = Clamp(relDip, 0.0, 1.0);
    }

    int newYPhys = (int)std::llround(newYDip * to.scale);

    // clamp within target monitor's physical bounds
    if (newYPhys < to.phys.top) newYPhys = to.phys.top;
    if (newYPhys >= to.phys.bottom) newYPhys = to.phys.bottom - 1;

    int newXPhys = leftToRight ? to.phys.left + 2 : to.phys.right - 2;

    if (g_debug) {
        std::printf("  srcYPhys=%d srcYDip=%.1f relDip=%.4f -> newYDip=%.1f newYPhys=%d\n",
            srcYPhys, srcYDip, relDip, newYDip, newYPhys);
    }

    g_warpInProgress = true;
    SetCursorPos(newXPhys, newYPhys);

    g_lastPt.x = newXPhys;
    g_lastPt.y = newYPhys;
    g_haveLast = true;
}

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
                if (g_debug)
                    std::printf("Cross L->R from (%ld,%ld) to (%d,%d)\n",
                        g_lastPt.x, g_lastPt.y, x, y);
                WarpDipAligned(true, g_lastPt.y);
                return 1;
            }

            if (g_lastPt.x >= g_boundaryX && x < g_boundaryX) {
                if (g_debug)
                    std::printf("Cross R->L from (%ld,%ld) to (%d,%d)\n",
                        g_lastPt.x, g_lastPt.y, x, y);
                WarpDipAligned(false, g_lastPt.y);
                return 1;
            }
        }

        g_lastPt.x = x;
        g_lastPt.y = y;
        g_haveLast = true;
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

static void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];

        if (a == "--debug") g_debug = true;
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
                std::puts("Invalid --mode. Use top|center.");
                std::exit(1);
            }
        } else {
            std::puts("Unknown arg.");
            std::printf("Usage:\n");
            std::printf("  mouse_fix.exe [--list] [--left N --right M] [--left-scale S --right-scale S] [--mode top|center] [--debug]\n");
            std::exit(1);
        }
    }
}

int main(int argc, char** argv) {
    ParseArgs(argc, argv);

    // Per-monitor DPI aware v2 so we get physical bounds + correct DPI
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    EnumerateMonitors();

    if (g_listOnly) {
        PrintMonitors();
        return 0;
    }

    SelectMonitors();
    PrintStartup();

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        std::printf("SetWindowsHookEx failed: %lu\n", GetLastError());
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    return 0;
}
