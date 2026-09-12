// ============================================================================
//  symbols.h —— 私有函数定位（脱离 Windhawk 的关键技术）
//
//  Windows 任务栏的 Taskbar.View.dll 里，改按钮宽度的函数是私有的、不导出。
//  Windhawk 靠它自带的符号库找到；本工具独立实现：运行时从微软公共符号服务器
//  下载对应的 PDB，再用 dbghelp 按名字定位。思路与开源项目 SlimBar11 一致。
//
//  代价：首次运行需要联网下载一次 PDB（几 MB，缓存在程序目录下的 symbols\，
//        卸载时随目录一并删除）。
// ============================================================================
#pragma once
#include <windows.h>
#include <string>

namespace teqw {

// PDB 下载进度回调：pct 为 0..100，拿不到总长度时给 -1。
using PdbProgressFn = void (*)(int pct, void* ctx);

// 在 mod 模块内按通配符（如 L"*UpdateButtonPadding*"）查找函数地址。
//
// preferContaining 是必须的：同一个函数名在多个类里都存在（本机 PDB 里有 7 个
//   UpdateButtonPadding），只靠通配符会随机命中不相干的类。传入类名关键字
//   （如 L"TaskListButton"）来锁定唯一目标。
//
// cacheDir 用于存放下载的 PDB。返回 nullptr 表示未找到。
// err（可选）会收到「选中了哪个符号」或失败原因，直接写进日志即可定位。
// onPdbProgress（可选）用于把下载进度报给调用方——这个 PDB 有 47 MB，
//   实测慢的时候要 10 分钟，没有进度提示用户会以为程序卡死了。
// outFromCache（可选）收到 PDB 是走本地缓存还是本次联网下载的。
//   之所以由本函数回报而不是调用方自己猜：PDB 文件名是从 PE 调试目录里读出来的，
//   调用方在此之前并不知道它叫什么，靠硬编码文件名去判断会得出错误结论。
void* ResolveSymbol(HMODULE mod, const wchar_t* wildcard, const wchar_t* preferContaining,
                    const std::wstring& cacheDir, std::wstring* err = nullptr,
                    PdbProgressFn onPdbProgress = nullptr, void* progressCtx = nullptr,
                    bool* outFromCache = nullptr);

}  // namespace teqw
