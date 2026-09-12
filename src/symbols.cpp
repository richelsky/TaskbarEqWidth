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
        out.name.assign(reinterpret_cast<const wchar_t*>(cv + 24));

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
// ---------------------------------------------------------------------------
bool HttpDownload(const wchar_t* host, const std::wstring& urlPath,
                  const std::wstring& outFile) {
    bool ok = false;
    HINTERNET hSession = WinHttpOpen(L"TaskbarEqWidth/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    WinHttpSetTimeouts(hSession, 10000, 10000, 20000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath.c_str(), nullptr,
                                                WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hRequest,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    HANDLE hOut = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hOut != INVALID_HANDLE_VALUE) {
                        DWORD total = 0;
                        BYTE buf[64 * 1024];
                        DWORD read = 0;
                        ok = true;
                        while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
                            DWORD written = 0;
                            if (!WriteFile(hOut, buf, read, &written, nullptr) || written != read) {
                                ok = false;
                                break;
                            }
                            total += written;
                        }
                        CloseHandle(hOut);
                        if (ok && total == 0) ok = false;  // 空文件视为失败
                    }
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);

    if (!ok) DeleteFileW(outFile.c_str());
    return ok;
}

// ---------------------------------------------------------------------------
//  3. 符号枚举回调
// ---------------------------------------------------------------------------
struct FindCtx {
    void* found = nullptr;
};

// 显式用宽字符版本（PSYMBOL_INFOW / SymEnumSymbolsW / SymInitializeW）。
// 用 PSYMBOL_INFO 这种 UNICODE 别名虽然通常也对，但别名映射是头文件的实现细节，
// 依赖它一旦踩空就是一堆类型不匹配的报错。PDB 里的符号名是 ASCII，用宽字符比较无碍。
BOOL CALLBACK EnumCallback(PSYMBOL_INFOW info, ULONG /*symbolSize*/, PVOID context) {
    auto* ctx = reinterpret_cast<FindCtx*>(context);
    const wchar_t* n = info->Name;
    // 跳过编译器生成的胶水代码，只取真正的实现函数
    if (wcsstr(n, L"dtor$") || wcsstr(n, L"thunk") || wcsstr(n, L"Adjustor") ||
        wcsstr(n, L"vftable") || wcsstr(n, L"catch$")) {
        return TRUE;
    }
    ctx->found = reinterpret_cast<void*>(info->Address);
    return FALSE;  // 找到第一个即可收工
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
void* ResolveSymbol(HMODULE mod, const wchar_t* wildcard, const std::wstring& cacheDir,
                    std::wstring* err) {
    auto fail = [&](const wchar_t* why, const std::wstring& detail) -> void* {
        if (err) *err = std::wstring(why) + (detail.empty() ? L"" : L" | " + detail);
        return nullptr;
    };
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
    if (!fromCache) {
        std::wstring url = L"/download/symbols/" + pdb.name + L"/" + pdb.key + L"/" + pdb.name;
        if (!HttpDownload(L"msdl.microsoft.com", url, pdbPath)) {
            return fail(L"PDB 下载失败（网络不通或符号服务器拒绝）", pdb.name);
        }
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

    FindCtx ctx;
    if (!SymEnumSymbolsW(GetCurrentProcess(), symBase, wildcard, EnumCallback, &ctx) || !ctx.found) {
        if (err) {
            *err = std::wstring(L"在 PDB 里没找到匹配的符号: ") + wildcard;
            *err += fromCache ? L"（用的是本地缓存）" : L"（刚下载）";
        }
        return nullptr;
    }
    return ctx.found;
}

}  // namespace teqw
