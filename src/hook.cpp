// ============================================================================
//  hook.cpp —— TaskbarEqWidthHook.dll  （x64）
//
//  这个 DLL 会被注入到 explorer.exe，做一件事：
//      把任务栏每个按钮的 IconPanel 宽度固定成同一个值 —— 也就是"等宽"。
//
//  原理（照搬 Windhawk 那个模块的公开思路，但只保留等宽这一条）：
//      Taskbar.View.dll 里 TaskListButton::UpdateButtonPadding 每次布局都会调用，
//      我们在它执行完之后，把按钮 XAML 元素里的 IconPanel.Width 改成固定值。
//
//  ---------------------------------------------------------------------------
//  干净卸载（本文件最重要的部分）：
//      1. 管理器 SetEvent(TEQW_EV_UNLOAD)
//      2. 本线程被唤醒 -> 置 g_unloading，触发任务栏重排，让宽度恢复成系统原生值
//      3. MH_DisableHook + MH_Uninitialize，把内存里所有 trampoline 撤销
//      4. SetEvent(TEQW_EV_UNLOADED) 通知管理器
//      5. FreeLibraryAndExitThread 把自己从 explorer 里彻底卸载
//      6. 管理器删除文件与注册表
//  全程不给 explorer.exe 打任何持久补丁，不改它的磁盘文件，不改它的注册表配置。
// ============================================================================

#include <windows.h>

#include <unknwn.h>
#undef GetCurrentTime  // windows.h 与 winrt 的宏冲突

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <wchar.h>

#include "shared.h"
#include "symbols.h"

#pragma comment(lib, "windowsapp.lib")

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;

// ---------------------------------------------------------------------------
//  全局状态
// ---------------------------------------------------------------------------
// 注意类型：HINSTANCE 与 HMODULE 在 C++ 里是两个不同的类型，MSVC 不会隐式转换。
// 窗口类注册/创建要 HINSTANCE，GetModuleFileNameW/FreeLibrary 要 HMODULE，
// 所以这里存 HINSTANCE，用到 HMODULE 的地方显式 reinterpret_cast。
static HINSTANCE                  g_hinst          = nullptr;
static std::atomic<bool>          g_unloading{false};
static std::atomic<int>           g_itemWidth{TEQW_DEFAULT_WIDTH};
static DWORD                      g_statusFromCache = 0;

static HANDLE                     g_evUnload       = nullptr;
static HANDLE                     g_evUnloaded     = nullptr;
static HANDLE                     g_evInitDone     = nullptr;
static HANDLE                     g_hMapStatus     = nullptr;
static TeqwStatus*                g_pStatus        = nullptr;
static UINT                       g_msgUnload      = 0;

static void*                      g_target         = nullptr;  // UpdateButtonPadding 地址
using UpdateButtonPadding_t = void(WINAPI*)(void*);
static UpdateButtonPadding_t      g_origUpdateButtonPadding = nullptr;

// DLL 自身所在目录，用于放置 PDB 缓存
static std::wstring               g_selfDir;

// ---------------------------------------------------------------------------
//  日志：写在 DLL 自己所在目录下的 hook.log
//
//  为什么需要它：钩子失败的原因可能有好几种（找不到 DLL、PDB 下不来、
//  符号名变了、杀软拦了 CreateRemoteThread），而 explorer 里没人能告诉你。
//  写成一个普通文本文件，卸载时随目录一起删除，不留残留物。
//  只在初始化/卸载这种一次性路径上调用，不进热路径。
// ---------------------------------------------------------------------------
static void LogReset() {
    if (g_selfDir.empty()) return;
    std::wstring p = g_selfDir + L"\\hook.log";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void LogLine(const wchar_t* fmt, ...) {
    if (g_selfDir.empty()) return;
    std::wstring p = g_selfDir + L"\\hook.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[1024];
    int n = swprintf_s(line, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnwprintf_s(line + n, 1024 - static_cast<size_t>(n), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > 1020) n = 1020;
    line[n++] = L'\r';
    line[n++] = L'\n';

    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(n * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
//  XAML 辅助：按名字在可视化树里找子元素
// ---------------------------------------------------------------------------
static FrameworkElement FindChildByName(DependencyObject const& root,
                                        std::wstring_view name) {
    int count = VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        auto child = VisualTreeHelper::GetChild(root, i);
        auto fe = child.try_as<FrameworkElement>();
        if (fe && std::wstring_view(fe.Name().c_str()) == name) return fe;
        auto deeper = FindChildByName(child, name);
        if (deeper) return deeper;
    }
    return nullptr;
}

// 只在头几次写日志，避免热路径把 hook.log 刷爆
static std::atomic<int> g_logMissingPanel{0};
static std::atomic<int> g_logApplied{0};

// ---------------------------------------------------------------------------
//  核心：给一个任务栏按钮应用固定宽度
// ---------------------------------------------------------------------------
static void ApplyFixedWidthToButton(void* pThis) {
    // WinRT 实现对象的外层 IUnknown 固定在第 4 个指针位置（模块公开代码同款做法）
    void* iunknownPtr = reinterpret_cast<void**>(pThis) + 3;
    winrt::Windows::Foundation::IUnknown iunk;
    winrt::copy_from_abi(iunk, iunknownPtr);

    auto buttonElement = iunk.as<FrameworkElement>();
    if (!buttonElement) return;

    auto iconPanel = FindChildByName(buttonElement, L"IconPanel");
    if (!iconPanel) {
        // 找到了函数、也挂上了钩子，但按钮里没有叫 IconPanel 的元素，
        // 就会表现成"日志说成功、任务栏毫无变化"。这种情况必须留下痕迹。
        if (g_logMissingPanel.fetch_add(1) < 3) {
            LogLine(L"[!] 按钮里没有名为 IconPanel 的子元素——界面结构可能变了，等宽不会生效");
        }
        return;
    }

    // 目标值：卸载中则恢复成 NaN（= 交还系统按内容自动计算）
    double want = g_unloading.load()
                      ? std::numeric_limits<double>::quiet_NaN()
                      : static_cast<double>(g_itemWidth.load());

    // 值没变就不写。XAML 属性写入会触发一次布局，而布局又会回调本函数，
    // 这里过滤掉无效写入，可以少掉大量无谓的重排。
    double cur = iconPanel.Width();
    bool same = (cur == want) ||
                (std::isnan(cur) && std::isnan(want));
    if (!same) {
        iconPanel.Width(want);
        if (g_logApplied.fetch_add(1) < 1) {
            LogLine(L"[OK] 已开始对任务栏按钮应用固定宽度 %.0f DIP", want);
        }
    }
}

// ---------------------------------------------------------------------------
//  钩子函数
//
//  重入保护：改宽度会让 XAML 重新布局，而重新布局又会调用 UpdateButtonPadding，
//  也就是再次进入我们这个钩子。Windhawk 那个模块同样加了这类保护，并注明
//  "Without it, there's an infinite rerendering loop"。这里用原子量做闸门
//  （XAML 布局是单线程的，原子量足够，而且不像 thread_local 那样在 DLL 卸载时
//   需要跑析构，注入场景下更安全）。
// ---------------------------------------------------------------------------
static std::atomic<bool> g_inHook{false};

static void WINAPI Hook_UpdateButtonPadding(void* pThis) {
    if (g_inHook.exchange(true)) {
        // 重入：只放行原函数，不再改宽度
        g_origUpdateButtonPadding(pThis);
        return;
    }

    g_origUpdateButtonPadding(pThis);

    try {
        ApplyFixedWidthToButton(pThis);
    } catch (...) {
        // 任务栏布局过渡期可能抛 WinRT 异常，吞掉，绝不能让它冒泡回 explorer
    }

    g_inHook.store(false);
}

// ---------------------------------------------------------------------------
//  消息窗口：作为"DLL 当前是否驻留"的标记，同时兜底接收卸载指令
// ---------------------------------------------------------------------------
static LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_msgUnload != 0 && msg == g_msgUnload) {
        if (g_evUnload) SetEvent(g_evUnload);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static HWND CreateMsgHostWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MsgWndProc;
    wc.hInstance     = g_hinst;
    wc.lpszClassName = TEQW_MSG_WINDOW_CLASS;
    RegisterClassExW(&wc);  // 已注册会失败，忽略

    g_msgUnload = RegisterWindowMessageW(L"TaskbarEqWidth_UnloadMsg");

    // 注意：这里刻意不用 HWND_MESSAGE。message-only 窗口无法被 FindWindow 找到，
    // 管理器就无从判断"DLL 是否还在 explorer 里"，卸载确认会变成一句空话。
    // 改用隐藏的顶层窗口（不调用 ShowWindow，不占屏幕、不进 Alt+Tab）。
    return CreateWindowExW(0, TEQW_MSG_WINDOW_CLASS, L"", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, g_hinst, nullptr);
}

// ---------------------------------------------------------------------------
//  读取配置（HKCU\Software\TaskbarEqWidth\ItemWidth）
// ---------------------------------------------------------------------------
static void LoadConfig() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TEQW_REG_CFG_KEY, 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS) {
        DWORD value = 0, size = sizeof(value), type = 0;
        if (RegQueryValueExW(hKey, TEQW_REG_VAL_WIDTH, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && value >= TEQW_MIN_WIDTH && value <= TEQW_MAX_WIDTH) {
            g_itemWidth.store(static_cast<int>(value));
        }
        RegCloseKey(hKey);
    }
}

// ---------------------------------------------------------------------------
//  等待 Taskbar.View.dll 被 explorer 载入
// ---------------------------------------------------------------------------
static HMODULE WaitForTaskbarViewDll(int timeoutMs) {
    for (int waited = 0; waited < timeoutMs; waited += 200) {
        HMODULE m = GetModuleHandleW(L"Taskbar.View.dll");
        if (!m) m = GetModuleHandleW(L"ExplorerExtensions.dll");
        if (m) return m;
        Sleep(200);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
//  卸载流程
// ---------------------------------------------------------------------------
static void RestoreAndUnhook() {
    g_unloading.store(true);

    // 触发任务栏重新布局：我们的钩子会在这轮里把宽度改回原生值
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (hTray) {
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(hTray, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, &unused);
        Sleep(400);
        SendMessageTimeoutW(hTray, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, &unused);
    }

    if (g_origUpdateButtonPadding) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_target));
    }
    MH_Uninitialize();
}

// PDB 下载进度 -> 共享内存，供管理器的等待界面显示
static void OnPdbProgress(int pct, void* ctx) {
    auto* st = static_cast<TeqwStatus*>(ctx);
    if (st) st->downloadPct = (pct < 0) ? 0xFFFFFFFFu : static_cast<DWORD>(pct);
}

// ---------------------------------------------------------------------------
//  初始化线程（不能占用 loader lock，所以放在独立线程里做）
// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID) {
    LogReset();
    LogLine(L"=== TaskbarEqWidth 钩子已注入 explorer.exe (PID %lu) ===",
            GetCurrentProcessId());
    LogLine(L"DLL 目录: %s", g_selfDir.c_str());

    CreateMsgHostWindow();
    LoadConfig();
    LogLine(L"配置的按钮宽度: %d DIP", g_itemWidth.load());

    g_evUnload = CreateEventW(nullptr, TRUE, FALSE, TEQW_EV_UNLOAD);
    g_evInitDone = CreateEventW(nullptr, TRUE, FALSE, TEQW_EV_INITDONE);

    // 共享内存：向管理器汇报状态
    g_hMapStatus = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(TeqwStatus), TEQW_SHM_STATUS);
    if (g_hMapStatus) {
        g_pStatus = static_cast<TeqwStatus*>(
            MapViewOfFile(g_hMapStatus, FILE_MAP_WRITE, 0, 0, sizeof(TeqwStatus)));
        if (g_pStatus) {
            g_pStatus->magic = TEQW_STATUS_MAGIC;
            g_pStatus->hookOk = 0;
            g_pStatus->itemWidth = static_cast<DWORD>(g_itemWidth.load());
            g_pStatus->fromCache = 0;
            g_pStatus->initDone = 0;
            g_pStatus->downloadPct = 0xFFFFFFFFu;  // 尚未开始下载
        }
    }

    // ---- 定位并挂钩 --------------------------------------------------------
    HMODULE taskbarView = WaitForTaskbarViewDll(30000);
    if (!taskbarView) {
        LogLine(L"[x] 30 秒内没找到 Taskbar.View.dll / ExplorerExtensions.dll");
        LogLine(L"    可能这台机器的任务栏 UI 由别的模块实现，或版本差异较大。");
    } else {
        wchar_t modPath[MAX_PATH]{};
        GetModuleFileNameW(taskbarView, modPath, MAX_PATH);
        LogLine(L"Taskbar.View.dll: base=%p  path=%s",
                reinterpret_cast<void*>(taskbarView), modPath);

        std::wstring cacheDir = g_selfDir + L"\\symbols";
        bool fromCache = false;
        std::wstring err;
        g_target = teqw::ResolveSymbol(taskbarView, L"*UpdateButtonPadding*",
                                       L"TaskListButton", cacheDir, &err,
                                       OnPdbProgress, g_pStatus, &fromCache);
        g_statusFromCache = fromCache ? 1 : 0;
        LogLine(L"PDB 缓存: %s", fromCache ? L"已有，直接使用" : L"没有，本次联网下载");
        LogLine(L"符号定位: %s", err.empty() ? L"(无说明)" : err.c_str());
        if (!g_target) {
            LogLine(L"[x] 符号定位失败: %s", err.c_str());
            LogLine(L"    这个私有函数名可能在本机 Windows 版本上变了。");
        } else {
            LogLine(L"已定位 UpdateButtonPadding @ %p", g_target);

            MH_STATUS mh = MH_Initialize();
            LogLine(L"MH_Initialize -> %d", static_cast<int>(mh));
            if (mh == MH_OK) {
                MH_STATUS c = MH_CreateHook(g_target,
                                            reinterpret_cast<LPVOID>(&Hook_UpdateButtonPadding),
                                            reinterpret_cast<LPVOID*>(&g_origUpdateButtonPadding));
                LogLine(L"MH_CreateHook -> %d", static_cast<int>(c));
                if (c == MH_OK) {
                    MH_STATUS e = MH_EnableHook(g_target);
                    LogLine(L"MH_EnableHook -> %d", static_cast<int>(e));
                    if (e == MH_OK) {
                        if (g_pStatus) g_pStatus->hookOk = 1;
                        LogLine(L"[OK] 钩子已生效，任务栏按钮宽度将被固定为 %d",
                                g_itemWidth.load());
                    }
                }
            }
        }
    }

    if (g_pStatus) {
        g_pStatus->fromCache = g_statusFromCache;
        if (g_pStatus->downloadPct == 0xFFFFFFFFu) g_pStatus->downloadPct = 100;
        g_pStatus->initDone = 1;  // 告诉管理器：初始化流程跑完了，不用再等
    }
    LogLine(L"--- 初始化结束 (hookOk=%lu) ---",
            g_pStatus ? g_pStatus->hookOk : 0UL);
    if (g_evInitDone) SetEvent(g_evInitDone);

    // ---- 没挂上钩子就主动撤离，不留"已注入但没生效"的僵尸状态 -----------------
    // 那种僵尸状态有两个坏处：一是 --status 显示「DLL 驻留：是 / 钩子：未挂上」，
    // 看着像半成功，实际毫无作用；二是 explorer 会一直握着这个 DLL 文件，
    // 妨碍下次覆盖更新。撤离后，管理器的下一次运行就是一次干净的重新尝试。
    if (!g_pStatus || g_pStatus->hookOk != 1) {
        LogLine(L"[!] 钩子未生效，DLL 将自行撤离 explorer（本次未成功，已回滚干净）");
        // 给管理器留出读取状态的时间：它每秒轮询一次共享内存，这里等够几个周期再撤。
        for (int i = 0; i < 10; ++i) {
            if (g_evUnload && WaitForSingleObject(g_evUnload, 1000) == WAIT_OBJECT_0) break;
        }
        g_evUnloaded = CreateEventW(nullptr, TRUE, FALSE, TEQW_EV_UNLOADED);
        if (g_evUnloaded) {
            SetEvent(g_evUnloaded);
            Sleep(30);
            CloseHandle(g_evUnloaded);
        }
        if (g_pStatus) UnmapViewOfFile(g_pStatus);
        if (g_hMapStatus) CloseHandle(g_hMapStatus);
        if (g_evUnload) CloseHandle(g_evUnload);
        if (g_evInitDone) CloseHandle(g_evInitDone);
        LogLine(L"=== 已从 explorer.exe 撤离，任务栏保持原样 ===");
        FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(g_hinst), 0);
    }

    // ---- 常驻等待卸载指令 --------------------------------------------------
    if (g_evUnload) WaitForSingleObject(g_evUnload, INFINITE);

    LogLine(L"收到卸载指令，开始恢复...");
    RestoreAndUnhook();
    LogLine(L"钩子已撤销，宽度已交还系统原生计算");

    // 通知管理器：已卸载，可以删文件了
    g_evUnloaded = CreateEventW(nullptr, TRUE, FALSE, TEQW_EV_UNLOADED);
    if (g_evUnloaded) {
        SetEvent(g_evUnloaded);
        Sleep(50);  // 给管理器一点时间收到信号
        CloseHandle(g_evUnloaded);
    }

    if (g_pStatus) UnmapViewOfFile(g_pStatus);
    if (g_hMapStatus) CloseHandle(g_hMapStatus);
    if (g_evUnload) CloseHandle(g_evUnload);
    if (g_evInitDone) CloseHandle(g_evInitDone);

    LogLine(L"=== 即将从 explorer.exe 卸载自身 ===");

    // 把自己从 explorer.exe 里彻底卸掉
    FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(g_hinst), 0);
    return 0;
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);

        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(hinst), path, MAX_PATH);
        g_selfDir = path;
        size_t slash = g_selfDir.find_last_of(L'\\');
        if (slash != std::wstring::npos) g_selfDir.resize(slash);

        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
