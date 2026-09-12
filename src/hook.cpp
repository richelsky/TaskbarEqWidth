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
#include <limits>
#include <string>
#include <string_view>

#include "shared.h"
#include "symbols.h"

#pragma comment(lib, "windowsapp.lib")

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Media;

// ---------------------------------------------------------------------------
//  全局状态
// ---------------------------------------------------------------------------
static HMODULE                    g_hinst          = nullptr;
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
    if (!iconPanel) return;

    if (g_unloading.load()) {
        // 卸载中：恢复成系统原生（NaN = 自动计算）
        iconPanel.Width(std::numeric_limits<double>::quiet_NaN());
    } else {
        iconPanel.Width(static_cast<double>(g_itemWidth.load()));
    }
}

// ---------------------------------------------------------------------------
//  钩子函数
// ---------------------------------------------------------------------------
static void WINAPI Hook_UpdateButtonPadding(void* pThis) {
    g_origUpdateButtonPadding(pThis);

    try {
        ApplyFixedWidthToButton(pThis);
    } catch (...) {
        // 任务栏布局过渡期可能抛 WinRT 异常，吞掉，绝不能让它冒泡回 explorer
    }
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

// ---------------------------------------------------------------------------
//  初始化线程（不能占用 loader lock，所以放在独立线程里做）
// ---------------------------------------------------------------------------
static DWORD WINAPI InitThread(LPVOID) {
    CreateMsgHostWindow();
    LoadConfig();

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
        }
    }

    // ---- 定位并挂钩 --------------------------------------------------------
    HMODULE taskbarView = WaitForTaskbarViewDll(30000);
    if (taskbarView) {
        std::wstring cacheDir = g_selfDir + L"\\symbols";
        std::wstring pdbPath = cacheDir + L"\\Taskbar.View.pdb";
        g_statusFromCache =
            (GetFileAttributesW(pdbPath.c_str()) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;

        g_target = teqw::ResolveSymbol(taskbarView, L"*UpdateButtonPadding*", cacheDir);

        if (g_target && MH_Initialize() == MH_OK) {
            if (MH_CreateHook(g_target,
                              reinterpret_cast<LPVOID>(&Hook_UpdateButtonPadding),
                              reinterpret_cast<LPVOID*>(&g_origUpdateButtonPadding)) == MH_OK &&
                MH_EnableHook(g_target) == MH_OK) {
                if (g_pStatus) g_pStatus->hookOk = 1;
            }
        }
    }

    if (g_pStatus) g_pStatus->fromCache = g_statusFromCache;
    if (g_evInitDone) SetEvent(g_evInitDone);

    // ---- 常驻等待卸载指令 --------------------------------------------------
    if (g_evUnload) WaitForSingleObject(g_evUnload, INFINITE);

    RestoreAndUnhook();

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

    // 把自己从 explorer.exe 里彻底卸掉
    FreeLibraryAndExitThread(g_hinst, 0);
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
        GetModuleFileNameW(hinst, path, MAX_PATH);
        g_selfDir = path;
        size_t slash = g_selfDir.find_last_of(L'\\');
        if (slash != std::wstring::npos) g_selfDir.resize(slash);

        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
