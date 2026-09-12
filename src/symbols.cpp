// ============================================================================
//  symbols.cpp —— PDB 下载 + 符号解析（x64）
// ============================================================================
#include "symbols.h"

#include <dbghelp.h>
#include <psapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <wchar.h>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")

namespace teqw {
namespace {

// ---------------------------------------------------------------------------
//  1. 从 PE 的调试目录里读出 PDB 文件名与符号服务器用的 key
// ---------------------------------------------------------------------------
struct PdbInfo {
    std::wstring name;  // 例如 Taskbar.View.pdb
    std::wstring key;   // GUID + Age，例如 1A2B3C4D...1
};

bool ReadPdbInfo(HMODULE mod, PdbInfo& out) {
    auto base = reinterpret_cast<BYTE*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

    auto dbg = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    DWORD count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

    for (DWORD i = 0; i < count; ++i) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        auto cv = base + dbg[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4) != 0) continue;  // 只认 RSDS（PDB 7.0）

        GUID guid;
        memcpy(&guid, cv + 4, sizeof(GUID));
        DWORD age;
        memcpy(&age, cv + 20, sizeof(DWORD));

        // RSDS 里的 PDB 文件名是 **ASCII/UTF-8 窄字符串**（以 NUL 结尾），不是宽字符串。
        // 这里曾经写成 reinterpret_cast<const wchar_t*>(cv + 24)，直接按 UTF-16 读，
        // 结果是把 "Taskbar.View.pdb" 两两字节拼成了「慔歳慢⹲楖睥瀮扤」这种乱码。
        // 那个乱码随后被拼进下载 URL，请求打到符号服务器上立刻被拒（实测 0.44 秒就失败），
        // 而日志里只写「网络不通」—— 一个纯粹的编码错误被伪装成了网络问题。
        const char* narrowName = reinterpret_cast<const char*>(cv + 24);
        size_t narrowLen = 0;
        while (narrowLen < 260 && narrowName[narrowLen] != '\0') ++narrowLen;  // 字段上限 260
        if (narrowLen == 0) continue;
        std::string narrowStr(narrowName, narrowLen);
        out.name.assign(narrowStr.begin(), narrowStr.end());  // 逐字符窄转宽，ASCII 无损

        wchar_t key[64];
        swprintf_s(key, L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
                   guid.Data1, guid.Data2, guid.Data3,
                   guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                   guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7],
                   age);
        out.key = key;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  2. 极简 HTTPS 下载（WinHttp）
//
//  为什么要试三种代理模式：国内机器上常见的情况是系统里配了代理但代理不通，
//  或者反过来只有直连才通。只固定用一种模式的话，失败时给出的信息是
//  「网络不通」——用户根本无从下手。三种依次试，并把最后一次的真实错误码带回去。
// ---------------------------------------------------------------------------
const wchar_t* ProxyModeName(DWORD mode) {
    switch (mode) {
        case WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY: return L"自动代理";
        case WINHTTP_ACCESS_TYPE_DEFAULT_PROXY:   return L"默认代理";
        case WINHTTP_ACCESS_TYPE_NO_PROXY:        return L"直连";
        default:                                  return L"未知";
    }
}

bool HttpDownload(const wchar_t* host, const std::wstring& urlPath,
                  const std::wstring& outFile, PdbProgressFn onProgress, void* ctx,
                  std::wstring* diag) {
    const DWORD modes[] = {
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,  // 读系统代理设置（Win8.1+）
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,    // 同一件事的旧写法，兼容老系统
        WINHTTP_ACCESS_TYPE_NO_PROXY,         // 完全直连
    };

    DWORD lastErr = 0;      // 最后一次 WinHTTP 层错误码
    DWORD lastStatus = 0;   // 最后一次服务器返回的 HTTP 状态码

    for (DWORD mode : modes) {
        bool ok = false;
        DWORD status = 0;
        lastErr = 0;

        HINTERNET hSession = WinHttpOpen(L"TaskbarEqWidth/1.0", mode,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            lastErr = GetLastError();
            continue;
        }
        // 解析/连接各 15 秒，发送 30 秒；接收给 120 秒——这个 PDB 有 47 MB，
        // 慢速网络下单次读取也可能停顿较久，超时给太紧会把正常的慢下载掐断。
        WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 120000);

        HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            lastErr = GetLastError();
        } else {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath.c_str(), nullptr,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE);
            if (!hRequest) {
                lastErr = GetLastError();
            } else {
                if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                    lastErr = GetLastError();
                } else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
                    lastErr = GetLastError();
                } else {
                    DWORD sz = sizeof(status);
                    WinHttpQueryHeaders(hRequest,
                                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                        WINHTTP_NO_HEADER_INDEX);
                    if (status == 200) {
                        // 取总长度以便报进度（拿不到就给 -1）
                        unsigned long long contentLen = 0;
                        DWORD lenSz = sizeof(contentLen);
                        if (!WinHttpQueryHeaders(hRequest,
                                                 WINHTTP_QUERY_CONTENT_LENGTH |
                                                     WINHTTP_QUERY_FLAG_NUMBER64,
                                                 WINHTTP_HEADER_NAME_BY_INDEX, &contentLen,
                                                 &lenSz, WINHTTP_NO_HEADER_INDEX)) {
                            contentLen = 0;
                        }
                        if (onProgress) onProgress(contentLen ? 0 : -1, ctx);

                        HANDLE hOut = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (hOut == INVALID_HANDLE_VALUE) {
                            lastErr = GetLastError();
                        } else {
                            DWORD total = 0;
                            BYTE buf[64 * 1024];
                            DWORD read = 0;
                            int lastPct = -1;
                            ok = true;
                            while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
                                DWORD written = 0;
                                if (!WriteFile(hOut, buf, read, &written, nullptr) ||
                                    written != read) {
                                    lastErr = GetLastError();
                                    ok = false;
                                    break;
                                }
                                total += written;
                                if (onProgress && contentLen) {
                                    int pct = static_cast<int>(
                                        (static_cast<unsigned long long>(total) * 100) / contentLen);
                                    if (pct > 100) pct = 100;
                                    if (pct != lastPct) {
                                        lastPct = pct;
                                        onProgress(pct, ctx);
                                    }
                                }
                            }
                            if (ok && total == 0) {
                                lastErr = ERROR_HANDLE_EOF;
                                ok = false;   // 空文件视为失败
                            }
                            CloseHandle(hOut);
                        }
                    } else {
                        lastStatus = status;
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);

        if (ok) {
            if (diag) *diag = std::wstring(L"下载成功（") + ProxyModeName(mode) + L"）";
            return true;
        }
        DeleteFileW(outFile.c_str());

        // 服务器已经给了 HTTP 应答，说明链路是通的，换代理模式没有意义
        if (lastStatus) break;
    }

    if (diag) {
        wchar_t buf[256];
        if (lastStatus) {
            swprintf_s(buf, L"三种代理模式均被拒，最后一次服务器返回 HTTP %lu", lastStatus);
        } else {
            swprintf_s(buf, L"三种代理模式均连不上，最后一次 WinHTTP 错误码 %lu", lastErr);
        }
        *diag = buf;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  3. 符号枚举：先把所有匹配收集起来，再由调用方按类名筛选
//
//  为什么不能"取第一个匹配"：这个 PDB 里有 7 个不同的类各有一个同名的
//  UpdateButtonPadding（TaskListButton / SearchBoxButton / OverflowToggleButton /
//  AugmentedEntryPointButton / ExperienceToggleButton / SearchBoxLaunchListButton …），
//  宽松通配符能把它们全匹配上，而枚举顺序没有任何保证。取第一个的后果是：
//  钩子挂上了、日志显示成功，但任务栏毫无变化——最难排查的一类失败。
// ---------------------------------------------------------------------------
struct EnumCtx {
    std::vector<std::wstring> names;
    std::vector<void*> addrs;
};

BOOL CALLBACK EnumCollect(PSYMBOL_INFOW info, ULONG /*symbolSize*/, PVOID context) {
    auto* c = static_cast<EnumCtx*>(context);
    if (c->names.size() >= 64) return FALSE;  // 收集够了，收工
    c->names.emplace_back(info->Name);
    c->addrs.push_back(reinterpret_cast<void*>(info->Address));
    return TRUE;
}

// SymInitialize 每进程只能有效调用一次
bool EnsureSymInitialized(const std::wstring& cacheDir) {
    static bool s_inited = false;
    static std::wstring s_dir;
    if (s_inited && s_dir == cacheDir) return true;

    if (!s_inited) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
        if (!SymInitializeW(GetCurrentProcess(), cacheDir.c_str(), FALSE)) return false;
        s_inited = true;
        s_dir = cacheDir;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  对外接口
// ---------------------------------------------------------------------------
void* ResolveSymbol(HMODULE mod, const wchar_t* wildcard, const wchar_t* preferContaining,
                    const std::wstring& cacheDir, std::wstring* err,
                    PdbProgressFn onPdbProgress, void* progressCtx, bool* outFromCache) {
    auto fail = [&](const wchar_t* why, const std::wstring& detail) -> void* {
        if (err) *err = std::wstring(why) + (detail.empty() ? L"" : L" | " + detail);
        return nullptr;
    };
    if (err) err->clear();
    if (!mod || !wildcard) return fail(L"参数无效", L"");
    if (cacheDir.empty()) return fail(L"缓存目录为空", L"");

    CreateDirectoryW(cacheDir.c_str(), nullptr);

    // 1) 从 DLL 的调试目录里读出 PDB 文件名与符号服务器用的 key
    PdbInfo pdb;
    if (!ReadPdbInfo(mod, pdb)) {
        return fail(L"读不出 DLL 的 PDB 信息（调试目录缺失或不是 RSDS 格式）", L"");
    }

    // 2) 该 PDB 若未缓存就先下载（这个 DLL 的 PDB 有数十 MB，首次会慢）
    std::wstring pdbPath = cacheDir + L"\\" + pdb.name;
    bool fromCache = (GetFileAttributesW(pdbPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (outFromCache) *outFromCache = fromCache;
    if (!fromCache) {
        std::wstring url = L"/download/symbols/" + pdb.name + L"/" + pdb.key + L"/" + pdb.name;
        std::wstring dlDiag;
        if (!HttpDownload(L"msdl.microsoft.com", url, pdbPath, onPdbProgress, progressCtx,
                          &dlDiag)) {
            return fail(L"PDB 下载失败", pdb.name + L"  [" + dlDiag + L"]");
        }
        if (err) *err = L"已下载符号文件: " + pdb.name;
    }

    if (!EnsureSymInitialized(cacheDir)) {
        return fail(L"SymInitialize 初始化失败", cacheDir);
    }

    // 3) 告诉 dbghelp 这个模块在内存里的基址，让它去 cacheDir 里找符号。
    //    注意 ImageName 传的是 **DLL 自己的路径**而不是 PDB 路径：
    //    dbghelp 会自己读 DLL 的调试目录算出 PDB 名与 key，再到搜索路径里取，
    //    这是最标准、最不容易出错的方式（PDB 路径那种写法只是后备）。
    wchar_t modPath[MAX_PATH]{};
    if (GetModuleFileNameW(mod, modPath, MAX_PATH) == 0) {
        modPath[0] = 0;
    }

    // 先卸载同基址的旧记录，避免重复加载
    SymUnloadModule64(GetCurrentProcess(), reinterpret_cast<DWORD64>(mod));

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) {
        return fail(L"GetModuleInformation 失败", L"");
    }

    DWORD64 symBase = 0;
    if (modPath[0]) {
        symBase = SymLoadModuleExW(GetCurrentProcess(), nullptr, modPath, nullptr,
                                   reinterpret_cast<DWORD64>(mi.lpBaseOfDll),
                                   mi.SizeOfImage, nullptr, 0);
    }
    if (symBase == 0) {
        // 后备：直接把 PDB 文件当成 ImageName 交给 dbghelp
        symBase = SymLoadModuleExW(GetCurrentProcess(), nullptr, pdbPath.c_str(), nullptr,
                                   reinterpret_cast<DWORD64>(mi.lpBaseOfDll),
                                   mi.SizeOfImage, nullptr, 0);
    }
    if (symBase == 0) {
        return fail(L"SymLoadModuleEx 失败（PDB 与模块不匹配或 PDB 损坏）", pdb.name);
    }

    // 收集所有匹配，再按类名挑出我们要的那一个（见 EnumCtx 的注释）
    EnumCtx c;
    if (!SymEnumSymbolsW(GetCurrentProcess(), symBase, wildcard, EnumCollect, &c)) {
        return fail(L"SymEnumSymbols 枚举失败", wildcard);
    }
    if (c.names.empty()) {
        return fail(L"PDB 里没有任何符号匹配这个通配符", wildcard);
    }

    // 1) 优先：既匹配通配符、又含有指定类名、还不是编译器生成的胶水符号
    if (preferContaining) {
        for (size_t i = 0; i < c.names.size(); ++i) {
            const std::wstring& n = c.names[i];
            if (n.find(preferContaining) == std::wstring::npos) continue;
            if (n.find(L"dtor$") != std::wstring::npos) continue;
            if (n.find(L"thunk") != std::wstring::npos) continue;
            if (n.find(L"Adjustor") != std::wstring::npos) continue;
            if (n.find(L"catch$") != std::wstring::npos) continue;
            if (err) *err += (err->empty() ? L"" : L"  |  ") + std::wstring(L"选中符号: ") + n;
            return c.addrs[i];
        }
    }

    // 2) 退一步：只要不是胶水符号就用（此时把全部候选写进 err，方便排查）
    if (err) {
        *err += (err->empty() ? L"" : L"  |  ");
        *err += L"没有含 \"" + std::wstring(preferContaining ? preferContaining : L"") +
                L"\" 的候选，全部匹配为: ";
        for (size_t i = 0; i < c.names.size() && i < 12; ++i) {
            if (i) *err += L" | ";
            *err += c.names[i];
        }
    }
    for (size_t i = 0; i < c.names.size(); ++i) {
        const std::wstring& n = c.names[i];
        if (n.find(L"dtor$") != std::wstring::npos) continue;
        if (n.find(L"thunk") != std::wstring::npos) continue;
        if (n.find(L"Adjustor") != std::wstring::npos) continue;
        if (n.find(L"catch$") != std::wstring::npos) continue;
        if (err) *err += L"  [退化采用] " + n;
        return c.addrs[i];
    }

    if (err) *err += L"  （全部都是胶水符号，放弃）";
    return nullptr;
}

}  // namespace teqw
