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

// 在 mod 模块内按通配符（如 L"*UpdateButtonPadding*"）查找第一个匹配的函数地址。
// cacheDir 用于存放下载的 PDB。返回 nullptr 表示未找到。
// err（可选）会收到失败原因，便于写日志定位到底是哪一步断了。
void* ResolveSymbol(HMODULE mod, const wchar_t* wildcard, const std::wstring& cacheDir,
                    std::wstring* err = nullptr);

}  // namespace teqw
