// ============================================================================
//  hook.cpp —— TaskbarEqWidthHook.dll  （x64）
//
//  这个 DLL 会被注入到 explorer.exe，做两件事：
//      1) 把任务栏每个按钮的宽度固定成同一个值 —— 也就是"等宽"；
//      2) 在任务栏最右侧永远留出一段空白（默认 120 DIP），否则按钮铺满整条
//         任务栏、鼠标无处可右键，也就点不出「任务栏设置」。
//
//  原理（照搬 Windhawk 那个模块的公开思路，但只保留等宽这一条）：
//      Taskbar.View.dll 里 TaskListButton::UpdateButtonPadding 每次布局都会调用，
//      我们在它执行完之后调整按钮 XAML 元素里的宽度。
//
//  Windows 有两套任务栏标签实现，按钮内部结构不同，所以这里有两条路径：
//      A. 系统的原生标签实现：IconPanel 是 2 列 Grid（图标列 + 标签列），
//         标签列是 Auto 宽度，直接改列宽会被系统重算回去，于是往该列塞一个
//         固定宽度的空 Border 把列"撑"住，并给标签文字设 MaxWidth 防止撑开。
//      B. 旧式实现：IconPanel 是普通面板，直接给它设 Width 即可。
//      走哪条在运行时按 IconPanel 的列数自动判定，并写进 hook.log。
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

// 头文件顺序不能随便改：Windows.Foundation.Collections.h 必须在所有 Xaml 头文件
// 之前。否则调用 IVector<T>::Size()/GetAt()/Append() 会报 C3779
// （"a function that returns 'auto' cannot be used before it is defined"）——
// 因为 consume_*IVector 的方法定义在 Collections.h 里，而 Xaml 头文件会在这里
// 之前就把 IVector<ColumnDefinition> / IVector<UIElement> 实例化掉。
// Windhawk 的 taskbar-labels 模块用的是同一个顺序。
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Controls.h>

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

// 用别名而不是 using namespace：Media 与 Controls 里有若干同名类型，
// 同时 using 进来会让重载解析变得难以预测。别名只解决书写长度，不引入歧义。
namespace Controls = winrt::Windows::UI::Xaml::Controls;

// ---------------------------------------------------------------------------
//  全局状态
// ---------------------------------------------------------------------------
// 注意类型：HINSTANCE 与 HMODULE 在 C++ 里是两个不同的类型，MSVC 不会隐式转换。
// 窗口类注册/创建要 HINSTANCE，GetModuleFileNameW/FreeLibrary 要 HMODULE，
// 所以这里存 HINSTANCE，用到 HMODULE 的地方显式 reinterpret_cast。
static HINSTANCE                  g_hinst          = nullptr;
static std::atomic<bool>          g_unloading{false};
static std::atomic<int>           g_itemWidth{TEQW_DEFAULT_WIDTH};
static std::atomic<int>           g_reserved{TEQW_DEFAULT_RESERVED};
static DWORD                      g_statusFromCache = 0;

// ---------------------------------------------------------------------------
//  布局几何（只在 explorer 的 UI 线程上读写，因此不需要任何锁）
//
//  为什么需要它：把每个按钮都设成固定 176 DIP 时，若 "176 × 按钮数" 已经超过
//  任务栏剩下的空间，系统会把它按可用宽度压回去 —— 结果按钮正好铺满整条任务栏，
//  右侧一点空白都不剩，鼠标没有地方可以右键，也就点不出「任务栏设置」。
//
//  解决思路：先标定出"按钮区总可用宽度"，再让 每个按钮 = 可用宽度/按钮数 - 预留/按钮数，
//  这样无论开多少个窗口，末尾始终空出约 `预留` 这么多 DIP。
//
//  标定只认一个证据：任务栏自己刚刚写进按钮的宽度。它要么等于我们刚写进去的值
//  （说明系统没在压缩我们，读数无效），要么小于我们设的上限（说明系统在按可用空间
//  压缩，这个值就是"可用宽度 / 按钮数"）。用这条规则就自然避开了自我反馈的死循环。
// ---------------------------------------------------------------------------
static double                     g_available      = 0.0;  // 按钮区可用总宽（DIP），0 = 未标定
static double                     g_lastApplied    = -1.0; // 上一次写入的宽度
static bool                       g_appliedOnce    = false;// 是否已经应用过一轮（之后读数才可信）
static bool                       g_outerWritable  = true; // 是否还能由我们决定按钮外层宽度
static std::atomic<unsigned>      g_pubWidth{0u};          // 供共享内存展示
static std::atomic<unsigned>      g_pubAvail{0u};
static std::atomic<unsigned>      g_pubCount{0u};

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
static std::atomic<int> g_logPath{0};

// ---------------------------------------------------------------------------
//  两条路径：Windows 有两套任务栏标签实现，按钮内部结构完全不同，必须分别处理。
//  只做一条的话，会出现"钩子挂上了、日志说成功、任务栏毫无变化"——最难查的失败。
//
//  判据（与 Windhawk taskbar-labels 模块一致）：带原生标签实现的系统里，
//  IconPanel 是一个 **2 列 Grid**（第 0 列图标、第 1 列标签文字）；
//  而旧实现里 IconPanel 是普通面板。
// ---------------------------------------------------------------------------
static const wchar_t* kSpacerName = L"TaskbarEqWidthSpacer";

// 路径 B：普通面板 —— 直接给 IconPanel 定宽即可。返回是否真的改动了。
static bool ApplyWidthPlainPanel(FrameworkElement const& iconPanel, double want) {
    double cur = iconPanel.Width();
    bool same = (cur == want) || (std::isnan(cur) && std::isnan(want));
    if (same) return false;
    iconPanel.Width(want);
    return true;
}

// 路径 A：2 列 Grid（原生标签）—— 标签列是 Auto 宽度，直接改列宽会被系统的
// 布局逻辑重算回去，所以改用"撑"的办法：往标签列塞一个固定宽度的空 Border，
// 列是 Auto，会自动长到这个宽度；同时给标签文字设 MaxWidth，防止长标题把列撑开。
// 卸载时把 Border 宽度归零，布局即恢复原样。
// 这里的套路与 Windhawk 模块的 WindhawkLabelSpacer 完全相同（它也不删元素只归零，
// 因为删除元素会引发布局异常）。
static bool ApplyWidthLabelGrid(FrameworkElement const& buttonElement,
                                Controls::Grid const& grid, double want) {
    const bool unloading = std::isnan(want);   // 卸载时调用方传的就是 NaN

    auto cols = grid.ColumnDefinitions();
    if (cols.Size() < 2u) return false;

    auto iconElement = FindChildByName(grid, L"Icon");
    if (!iconElement) return false;   // 结构不符，交给调用方记录

    auto padding = grid.Padding();
    double firstCol = 0.0;
    auto c0 = cols.GetAt(0).Width();
    if (c0.GridUnitType == GridUnitType::Pixel) firstCol = c0.Value;

    // 目标：图标列 + 标签列 + 左右内边距 == want
    double labelCol = want - firstCol - padding.Left - padding.Right;
    if (!unloading && labelCol < 1.0) labelCol = 1.0;
    double spacerWant = unloading ? 0.0 : labelCol;

    auto spacer = FindChildByName(grid, kSpacerName);
    if (!spacer) {
        if (unloading) return false;   // 卸载时若没建过就不用建
        Controls::Border b;
        b.Name(kSpacerName);
        b.Height(0);
        Controls::Grid::SetColumn(b, 1);
        grid.Children().Append(b);
        spacer = b;
    }

    bool changed = false;
    if (spacer.Width() != spacerWant) {
        spacer.Width(spacerWant);
        changed = true;
    }

    if (auto label = FindChildByName(grid, L"LabelControl").try_as<Controls::TextBlock>()) {
        auto m = label.Margin();
        // 加载时把标签限制在列宽内（超长标题变省略号）；卸载时交还默认值，
        // 注意这里必须是 infinity（= XAML 的默认 MaxWidth）而不是 0，
        // 写 0 会把标签文字整个裁没。
        double maxW = unloading ? std::numeric_limits<double>::infinity()
                                : std::fmax(0.0, spacerWant - m.Left - m.Right);
        if (label.MaxWidth() != maxW) {
            label.MaxWidth(maxW);
            buttonElement.InvalidateMeasure();
            changed = true;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
//  按钮计数
//
//  为什么要数按钮：预留的空白要按按钮数分摊成"每个按钮让出多少"，
//  所以需要一个当前按钮数量。这里用最朴素的办法——观察钩子被调用的次数：
//  任务栏每次重排都会对每个按钮调用一次 UpdateButtonPadding，
//  于是"5 秒窗口内见过的不同按钮对象"就是按钮数。
//
//  只增不减：窗口过期的瞬间若只见到 1 个按钮，按 1 去分摊会让那一个按钮
//  被压得极窄。所以这里取"本次会话见过的最大值"，宁可少留一点空白，
//  也不让按钮宽度突然崩掉。
// ---------------------------------------------------------------------------
static void* g_seen[128];
static int   g_seenN    = 0;
static DWORD g_seenTick = 0;
static int   g_countMax = 1;

static int NoteButton(void* pThis) {
    DWORD now = GetTickCount();
    if (now - g_seenTick > 5000) g_seenN = 0;   // 一个"重排窗口"
    g_seenTick = now;

    for (int i = 0; i < g_seenN; ++i) {
        if (g_seen[i] == pThis) {
            if (g_countMax < g_seenN) g_countMax = g_seenN;
            return g_countMax;
        }
    }
    if (g_seenN < 128) g_seen[g_seenN++] = pThis;
    if (g_countMax < g_seenN) g_countMax = g_seenN;
    return g_countMax;
}

// ---------------------------------------------------------------------------
//  标定"按钮区可用总宽"
//
//  规则见文件顶部注释：只接受"系统自己刚写进去、且与我们写入的值不同"的宽度。
//  另外要求它明显小于我们设的上限（否则我们分辨不出这是系统的自适应结果，
//  还是我们设的内容宽度被原样采纳了）。
//  变化小于 8 DIP 时不重新标定，避免来回抖动导致按钮宽度不稳。
// ---------------------------------------------------------------------------
static void CalibrateAvailable(FrameworkElement const& btn, int count) {
    if (!g_appliedOnce || count < 1) return;

    double sysW = btn.Width();
    if (!std::isfinite(sysW) || sysW < static_cast<double>(TEQW_MIN_WIDTH)) return;
    if (std::fabs(sysW - g_lastApplied) < 0.5) return;          // 是我们自己写的
    if (sysW >= static_cast<double>(g_itemWidth.load()) - 0.5) return;  // 没有压缩迹象

    // 走到这里说明：外层宽度是系统自己写进去的，我们写不过它。
    // 立刻放弃写外层——继续硬写会变成"每帧都在改"的无限重排。
    // 好消息是不需要它：把内容宽度压到可用宽度以下，系统自然会留出空白。
    if (g_outerWritable) {
        g_outerWritable = false;
        LogLine(L"[i] 外层宽度由系统掌控，改用只约束内容的方式（留白同样生效）");
    }

    double avail = sysW * count;
    if (g_available <= 0.0 || std::fabs(avail - g_available) > 8.0) {
        g_available = avail;
        LogLine(L"[i] 标定：系统给每个按钮 %.1f DIP × %d 个 = 可用宽度 %.1f DIP",
                sysW, count, avail);
        g_pubAvail.store(static_cast<unsigned>(avail + 0.5));
    }
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
    if (!iconPanel) {
        // 找到了函数、也挂上了钩子，但按钮里没有叫 IconPanel 的元素，
        // 就会表现成"日志说成功、任务栏毫无变化"。这种情况必须留下痕迹。
        if (g_logMissingPanel.fetch_add(1) < 3) {
            LogLine(L"[!] 按钮里没有名为 IconPanel 的子元素——界面结构可能变了，等宽不会生效");
        }
        return;
    }

    // ---- 目标宽度 --------------------------------------------------------
    // 卸载中 -> NaN（= 交还系统按内容自动计算）
    // 正常 -> min(可用宽度, 上限宽度) - 预留/按钮数
    //         前半段保证按钮铺得下且等宽，后半段保证末尾留出空白。
    const bool unloading = g_unloading.load();
    const int  count     = unloading ? 1 : NoteButton(pThis);
    double     want;
    if (unloading) {
        want = std::numeric_limits<double>::quiet_NaN();
    } else {
        const double maxW = static_cast<double>(g_itemWidth.load());
        CalibrateAvailable(buttonElement, count);

        double perButton = (g_available > 0.0) ? (g_available / count) : maxW;
        if (perButton > maxW) perButton = maxW;   // 按钮很少时不要超出上限

        want = perButton - static_cast<double>(g_reserved.load()) / count;
        if (want < TEQW_MIN_WIDTH) want = TEQW_MIN_WIDTH;
        if (want > maxW) want = maxW;

        g_pubCount.store(static_cast<unsigned>(count));
        g_pubWidth.store(static_cast<unsigned>(want + 0.5));
        if (g_logApplied.load() == 0) {
            LogLine(L"[i] 宽度: 可用=%s 按钮数=%d 预留=%d -> 每个按钮 %.1f DIP",
                    g_available > 0.0 ? L"已标定" : L"未标定(先用上限)",
                    count, g_reserved.load(), want);
        }
    }

    // 先判断本机走哪条路径，并把结论写进日志（只写一次）。
    bool isLabelGrid = false;
    Controls::Grid grid{nullptr};
    if (auto g = iconPanel.try_as<Controls::Grid>()) {
        try {
            if (g.ColumnDefinitions().Size() >= 2u) {
                isLabelGrid = true;
                grid = g;
            }
        } catch (...) {
            // 属性读取在布局过渡期可能抛异常，按普通面板处理
        }
    }
    if (g_logPath.fetch_add(1) < 1) {
        LogLine(L"[i] IconPanel 结构: %s",
                isLabelGrid ? L"2 列 Grid（系统的原生标签实现），用标签列占位法"
                            : L"普通面板（旧式实现），直接设置 IconPanel.Width");
    }

    // 两层一起改才稳：
    //   外层（按钮本体）—— 决定这个按钮最终占多宽，是"能不能留出空白"的关键。
    //                      系统的自适应布局会往这里写值，我们钩子跑在它之后，所以写得住。
    //   内层（IconPanel）—— 决定内容怎么排，避免标签把按钮撑开。
    // 若发现外层其实是系统在写（见 CalibrateAvailable），就只做内层。
    bool changed = false;
    if (unloading) {
        buttonElement.Width(std::numeric_limits<double>::quiet_NaN());
    } else if (g_outerWritable) {
        double curOuter = buttonElement.Width();
        bool sameOuter = (curOuter == want) || (std::isnan(curOuter) && std::isnan(want));
        if (!sameOuter) {
            buttonElement.Width(want);
            changed = true;
        }
    }

    if (isLabelGrid ? ApplyWidthLabelGrid(buttonElement, grid, want)
                    : ApplyWidthPlainPanel(iconPanel, want)) {
        changed = true;
    }

    if (!unloading) {
        g_lastApplied = want;   // 记住我们写的值，标定时用来区分"谁写的"
        g_appliedOnce = true;
    }

    if (changed && g_logApplied.fetch_add(1) < 1) {
        LogLine(L"[OK] 已开始对任务栏按钮应用固定宽度 %.0f DIP（右侧留白约 %d DIP）",
                want, g_reserved.load());
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
//  读取配置
//      HKCU\Software\TaskbarEqWidth\ItemWidth      按钮宽度上限
//      HKCU\Software\TaskbarEqWidth\ReservedWidth  右侧预留空白
//  每次读都返回是否有变化，用于"改完立刻生效"（不必重装）。
// ---------------------------------------------------------------------------
static bool LoadConfig() {
    bool changed = false;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TEQW_REG_CFG_KEY, 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS) {
        DWORD value = 0, size = sizeof(value), type = 0;

        size = sizeof(value);
        if (RegQueryValueExW(hKey, TEQW_REG_VAL_WIDTH, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && value >= TEQW_MIN_WIDTH && value <= TEQW_MAX_WIDTH) {
            if (g_itemWidth.load() != static_cast<int>(value)) {
                g_itemWidth.store(static_cast<int>(value));
                changed = true;
            }
        }

        value = 0;
        size  = sizeof(value);
        if (RegQueryValueExW(hKey, TEQW_REG_VAL_RESERVED, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD && value >= TEQW_MIN_RESERVED && value <= TEQW_MAX_RESERVED) {
            if (g_reserved.load() != static_cast<int>(value)) {
                g_reserved.store(static_cast<int>(value));
                changed = true;
            }
        }
        RegCloseKey(hKey);
    }
    return changed;
}

// 让任务栏整体重排一次，钩子就会用新参数重新写一遍宽度。
// 换分辨率、改配置后都需要这个，否则要等到用户下次点开某个窗口才会刷新。
static void NudgeTaskbar() {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return;
    DWORD_PTR unused = 0;
    SendMessageTimeoutW(hTray, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, &unused);
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
    NudgeTaskbar();
    Sleep(400);
    NudgeTaskbar();

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
    LogLine(L"配置: 按钮宽度上限 %d DIP，右侧预留空白 %d DIP",
            g_itemWidth.load(), g_reserved.load());

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
            g_pStatus->buttonCount = 0;
            g_pStatus->reserved = static_cast<DWORD>(g_reserved.load());
            g_pStatus->availWidth = 0;
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
                        LogLine(L"[OK] 钩子已生效（宽度上限 %d DIP，右侧预留 %d DIP）",
                                g_itemWidth.load(), g_reserved.load());
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

    // ---- 常驻：等卸载指令，同时盯着配置变化 --------------------------------
    // 每 700ms 扫一次注册表。改动（宽度上限 / 预留空白）会立刻触发一次任务栏重排，
    // 钩子随即用新参数重写宽度 —— 于是调参不需要重装、也不需要重启资源管理器。
    for (;;) {
        if (!g_evUnload) break;
        if (WaitForSingleObject(g_evUnload, 700) == WAIT_OBJECT_0) break;

        if (LoadConfig()) {
            LogLine(L"[i] 配置已变更：宽度上限 %d DIP，预留 %d DIP —— 立即重排任务栏",
                    g_itemWidth.load(), g_reserved.load());
            if (g_pStatus) g_pStatus->reserved = static_cast<DWORD>(g_reserved.load());
            NudgeTaskbar();
        }
        if (g_pStatus) {
            unsigned w = g_pubWidth.load();
            if (w > 0) g_pStatus->itemWidth = w;
            g_pStatus->buttonCount = g_pubCount.load();
            g_pStatus->availWidth  = g_pubAvail.load();
        }
    }

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
