// ============================================================================
//  manager.cpp —— TaskbarEqWidth.exe（安装 / 卸载 / 状态 / 残留扫描）
//
//  ---------------------------------------------------------------------------
//  为什么要把自己复制到 %LOCALAPPDATA%\TaskbarEqWidth\ 再运行？
//    因为"完全卸载"必然要删掉程序文件。如果就地删除，而你又恰好把它放在
//    桌面或某个资料夹里，就会误删你自己目录里的东西。所以：
//      安装 -> 复制到我们专属的目录，之后一切只围绕这个目录
//      卸载 -> 只删这个目录，其余一律不碰
//    这是本工具"干净卸载"承诺的第一条：不删除任何不属于它的文件。
//
//  ---------------------------------------------------------------------------
//  它在你电脑上留下的全部东西（一双手数得完）：
//    1) %LOCALAPPDATA%\TaskbarEqWidth\            <- 两个文件 + PDB 缓存
//    2) HKCU\Software\TaskbarEqWidth              <- 一个 DWORD：按钮宽度
//    3) HKCU\...\CurrentVersion\Run\TaskbarEqWidth<- 可选，仅 --autostart 时
//    4) explorer.exe 内存中临时驻留的 hook 代码    <- 退出即消失
//  除此之外：无服务、无驱动、无计划任务、无 HKLM、无系统目录、无 PATH 改动。
// ============================================================================

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>   // _wtoi
#include <string>
#include <string.h>  // _wcsicmp
#include <wchar.h>

#include "shared.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------
static std::wstring GetSelfDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t p = s.find_last_of(L'\\');
    return (p == std::wstring::npos) ? s : s.substr(0, p);
}

static std::wstring GetSelfPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// 唯一的"官方"安装目录
static std::wstring GetInstallDir() {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\" + TEQW_APP_NAME;
}

static bool FileExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return;
    }
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
}

static DWORD FindExplorerPid() {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTray) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hTray, &pid);
    return pid;
}

// ---------------------------------------------------------------------------
//  注册表
// ---------------------------------------------------------------------------
static bool RegSetDword(const wchar_t* subKey, const wchar_t* name, DWORD value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegSetValueExW(hKey, name, 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

static bool RegSetString(const wchar_t* subKey, const wchar_t* name,
                         const std::wstring& value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_WRITE, nullptr,
                        &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegSetValueExW(hKey, name, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

static DWORD RegGetDword(const wchar_t* subKey, const wchar_t* name, DWORD def) {
    HKEY hKey = nullptr;
    DWORD value = def;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(value), type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS ||
            type != REG_DWORD) {
            value = def;
        }
        RegCloseKey(hKey);
    }
    return value;
}

static void RegDeleteValueSafe(const wchar_t* subKey, const wchar_t* name) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, name);
        RegCloseKey(hKey);
    }
}

static void RegDeleteKeySafe(const wchar_t* subKey) {
    RegDeleteKeyW(HKEY_CURRENT_USER, subKey);  // 仅删空键，有子键则失败，不会误删
}

// ---------------------------------------------------------------------------
//  注入 / 卸载 通信
// ---------------------------------------------------------------------------
static bool InjectDll(DWORD pid, const std::wstring& dllPath) {
    if (!pid) return false;

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) return false;

    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        CloseHandle(hProc);
        return false;
    }
    if (!WriteProcessMemory(hProc, remote, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // 用 LoadLibraryW 而不是自写代码，属于最常见的注入方式
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    auto pLoadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hKernel, "LoadLibraryW"));

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pLoadLibraryW, remote, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    WaitForSingleObject(hThread, 15000);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return true;
}

// DLL 是否仍驻留在 explorer.exe 里。
// 两个独立证据取或：消息窗口存在，或共享内存段仍被持有。
// 任一为真都说明 DLL 还活着，卸载流程绝不能放过。
static bool IsDllResident() {
    if (FindWindowW(TEQW_MSG_WINDOW_CLASS, nullptr) != nullptr) return true;
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, TEQW_SHM_STATUS);
    if (hMap) {
        CloseHandle(hMap);
        return true;
    }
    return false;
}

// 请求 DLL 卸载，返回是否确认卸载完成
static bool RequestUnload(int timeoutMs) {
    if (!IsDllResident()) return true;

    HANDLE hUnloaded = CreateEventW(nullptr, TRUE, FALSE, TEQW_EV_UNLOADED);
    HANDLE hUnload = OpenEventW(EVENT_MODIFY_STATE, FALSE, TEQW_EV_UNLOAD);
    if (!hUnload) {
        if (hUnloaded) CloseHandle(hUnloaded);
        return false;
    }
    SetEvent(hUnload);
    CloseHandle(hUnload);

    // 兜底：直接给消息窗口发消息（万一事件路径失效）
    HWND hMsg = FindWindowW(TEQW_MSG_WINDOW_CLASS, nullptr);
    if (hMsg) {
        UINT m = RegisterWindowMessageW(L"TaskbarEqWidth_UnloadMsg");
        if (m) {
            DWORD_PTR unused = 0;
            SendMessageTimeoutW(hMsg, m, 0, 0, SMTO_NORMAL, 2000, &unused);
        }
    }

    bool ok = false;
    if (hUnloaded) {
        ok = WaitForSingleObject(hUnloaded, timeoutMs) == WAIT_OBJECT_0;
        CloseHandle(hUnloaded);
    }

    // DLL 收到信号后还要跑一段收尾（重排任务栏、撤销 trampoline），
    // 这里再给它最多 2 秒真正从 explorer 里消失。
    for (int i = 0; i < 20 && IsDllResident(); ++i) Sleep(100);

    return ok && !IsDllResident();
}

// 终极兜底：重启 explorer.exe。这会强行丢弃注入的 DLL，保证不留残骸。
static void RestartExplorer() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 0);
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    // Windows 会自动重启 explorer；等它回来
    for (int i = 0; i < 100; ++i) {
        if (FindWindowW(L"Shell_TrayWnd", nullptr)) break;
        Sleep(200);
    }
}

// ---------------------------------------------------------------------------
//  安装
// ---------------------------------------------------------------------------
static int DoInstall(DWORD width, bool autostart, bool quiet) {
    std::wstring installDir = GetInstallDir();
    CreateDirectoryW(installDir.c_str(), nullptr);
    if (!FileExists(installDir)) {
        fwprintf(stderr, L"[x] 无法创建安装目录: %s\n", installDir.c_str());
        return 1;
    }

    std::wstring selfPath = GetSelfPath();
    std::wstring selfDir = GetSelfDir();
    std::wstring targetExe = installDir + L"\\" + TEQW_MANAGER_EXE_NAME;
    std::wstring targetDll = installDir + L"\\" + TEQW_HOOK_DLL_NAME;
    std::wstring sourceDll = selfDir + L"\\" + TEQW_HOOK_DLL_NAME;

    // 1) 把 exe / dll 放到专属目录（若已在专属目录则跳过复制）
    if (_wcsicmp(selfPath.c_str(), targetExe.c_str()) != 0) {
        if (!CopyFileW(selfPath.c_str(), targetExe.c_str(), FALSE)) {
            fwprintf(stderr, L"[x] 复制主程序失败 (%lu)\n", GetLastError());
            return 1;
        }
    }
    if (_wcsicmp(sourceDll.c_str(), targetDll.c_str()) != 0) {
        if (!CopyFileW(sourceDll.c_str(), targetDll.c_str(), FALSE)) {
            fwprintf(stderr, L"[x] 找不到或无法复制 %s\n", TEQW_HOOK_DLL_NAME);
            fwprintf(stderr, L"    请确保它和主程序放在同一个文件夹。\n");
            return 1;
        }
    }
    if (!FileExists(targetDll)) {
        fwprintf(stderr, L"[x] 安装目录里缺少 %s\n", TEQW_HOOK_DLL_NAME);
        return 1;
    }

    // 2) 写入配置（卸载时这个键会被整体删除）
    if (width < TEQW_MIN_WIDTH) width = TEQW_MIN_WIDTH;
    if (width > TEQW_MAX_WIDTH) width = TEQW_MAX_WIDTH;
    RegSetDword(TEQW_REG_CFG_KEY, TEQW_REG_VAL_WIDTH, width);
    RegSetDword(TEQW_REG_CFG_KEY, TEQW_REG_VAL_MANAGED, 1);

    // 3) 若已在运行，先卸载旧的一份，保证重新注入的是最新代码
    if (IsDllResident()) {
        if (!quiet) wprintf(L"[*] 检测到正在运行，先卸载旧实例...\n");
        if (!RequestUnload(5000)) {
            if (!quiet) wprintf(L"[*] 卸载确认超时，改为重启资源管理器...\n");
            RestartExplorer();
        }
    }

    // 4) 注入
    EnableDebugPrivilege();
    DWORD pid = FindExplorerPid();
    if (!pid) {
        fwprintf(stderr, L"[x] 找不到 explorer.exe\n");
        return 1;
    }

    if (!quiet) wprintf(L"[*] 正在注入 explorer.exe (PID %lu)...\n", pid);
    if (!InjectDll(pid, targetDll)) {
        fwprintf(stderr, L"[x] 注入失败 (%lu)。请尝试以管理员身份运行。\n", GetLastError());
        return 1;
    }

    // 5) 等 DLL 完成初始化
    HANDLE hInit = OpenEventW(SYNCHRONIZE, FALSE, TEQW_EV_INITDONE);
    if (hInit) {
        WaitForSingleObject(hInit, 35000);  // 首次需下载 PDB，给足时间
        CloseHandle(hInit);
    }
    Sleep(300);

    // 6) 读取 DLL 回报的状态
    bool ok = false;
    int shownWidth = static_cast<int>(width);
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, TEQW_SHM_STATUS);
    if (hMap) {
        auto* st = static_cast<TeqwStatus*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0,
                                                          sizeof(TeqwStatus)));
        if (st && st->magic == TEQW_STATUS_MAGIC) {
            ok = st->hookOk != 0;
            shownWidth = static_cast<int>(st->itemWidth);
            if (!quiet) {
                wprintf(L"[*] 符号来源: %s\n", st->fromCache ? L"本地缓存" : L"在线下载");
            }
        }
        if (st) UnmapViewOfFile(st);
        CloseHandle(hMap);
    }

    // 7) 可选开机自启（HKCU Run，用户级，卸载一键清除）
    if (autostart) {
        std::wstring cmd = L"\"" + targetExe + L"\" --quiet";
        RegSetString(TEQW_REG_RUN_KEY, TEQW_REG_RUN_VALUE, cmd);
        if (!quiet) wprintf(L"[*] 已注册开机自启\n");
    }

    if (!quiet) {
        wprintf(L"\n");
        if (ok) {
            wprintf(L"[OK] 已生效 —— 任务栏按钮宽度统一为 %d\n", shownWidth);
            wprintf(L"     安装目录: %s\n", installDir.c_str());
            wprintf(L"     卸载命令: \"%s\" --uninstall\n", targetExe.c_str());
        } else {
            wprintf(L"[!] 已注入，但未能挂上钩子。\n");
            wprintf(L"    可能原因：杀毒软件拦截、Windows 版本不被支持、或网络不通导致\n");
            wprintf(L"    符号下载失败。请查看安装目录下的 symbols\\ 是否生成了 PDB。\n");
        }
    }
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
//  卸载（核心：干净、彻底）
// ---------------------------------------------------------------------------
static int DoUninstall(bool keepFiles, bool quiet) {
    std::wstring installDir = GetInstallDir();
    int problems = 0;

    // --- 安全闸门：只允许删除 LOCALAPPDATA 下我们自己的目录 ------------------
    wchar_t localAppData[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring expectPrefix = std::wstring(localAppData) + L"\\" + TEQW_APP_NAME;
    bool safeToDelete = (_wcsicmp(installDir.c_str(), expectPrefix.c_str()) == 0) &&
                        (std::wstring(localAppData).size() > 3);
    if (!safeToDelete) {
        fwprintf(stderr, L"[x] 安装目录校验不通过，拒绝删除任何文件：%s\n",
                 installDir.c_str());
        keepFiles = true;
        ++problems;
    }

    // --- 1) 让 DLL 从 explorer 里卸载 ---------------------------------------
    if (!quiet) wprintf(L"[1/5] 卸载注入的 DLL...\n");
    if (IsDllResident()) {
        if (!RequestUnload(8000)) {
            if (!quiet) wprintf(L"      确认超时，重启资源管理器强制清除...\n");
            RestartExplorer();
            Sleep(1000);
        }
    }
    if (IsDllResident()) {
        fwprintf(stderr, L"      [!] DLL 仍在 explorer 中\n");
        ++problems;
    } else if (!quiet) {
        wprintf(L"      完成，explorer 已恢复原生行为\n");
    }

    // --- 2) 开机自启 ---------------------------------------------------------
    if (!quiet) wprintf(L"[2/5] 移除开机自启项...\n");
    RegDeleteValueSafe(TEQW_REG_RUN_KEY, TEQW_REG_RUN_VALUE);

    // --- 3) 配置键 -----------------------------------------------------------
    if (!quiet) wprintf(L"[3/5] 删除注册表配置...\n");
    RegDeleteValueSafe(TEQW_REG_CFG_KEY, TEQW_REG_VAL_WIDTH);
    RegDeleteValueSafe(TEQW_REG_CFG_KEY, TEQW_REG_VAL_MANAGED);
    RegDeleteKeySafe(TEQW_REG_CFG_KEY);

    // --- 4) 程序文件 ---------------------------------------------------------
    if (!quiet) wprintf(L"[4/5] 删除程序文件...\n");
    if (keepFiles) {
        if (!quiet) wprintf(L"      已保留: %s\n", installDir.c_str());
    } else if (safeToDelete) {
        // 先删 PDB 缓存和 DLL，再删 EXE（正在运行的 exe 删不掉，交给最后一步）
        DeleteFileW((installDir + L"\\symbols\\Taskbar.View.pdb").c_str());
        RemoveDirectoryW((installDir + L"\\symbols").c_str());
        DeleteFileW((installDir + L"\\" + TEQW_HOOK_DLL_NAME).c_str());
        DeleteFileW((installDir + L"\\" + TEQW_MANAGER_EXE_NAME).c_str());
        RemoveDirectoryW(installDir.c_str());
    }

    // --- 5) 收尾 -------------------------------------------------------------
    if (!quiet) wprintf(L"[5/5] 校验残留...\n");

    bool leftover = false;
    if (FileExists(installDir)) leftover = true;
    if (RegGetDword(TEQW_REG_CFG_KEY, TEQW_REG_VAL_MANAGED, 0xFFFFFFFF) != 0xFFFFFFFF) {
        leftover = true;
    }
    if (IsDllResident()) leftover = true;

    if (leftover && !keepFiles && safeToDelete) {
        // 自己正占着 exe 文件，交给一个脱离的 cmd 延时清空整个目录
        std::wstring cmd = L"/c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + installDir + L"\"";
        HINSTANCE r = ShellExecuteW(nullptr, L"open", L"cmd.exe", cmd.c_str(), nullptr,
                                    SW_HIDE);
        if (reinterpret_cast<INT_PTR>(r) > 32) {
            if (!quiet) {
                wprintf(L"      已安排删除: %s（约 2 秒后完成）\n", installDir.c_str());
            }
        } else {
            fwprintf(stderr, L"      [!] 自动删除失败，请手动删除: %s\n",
                     installDir.c_str());
            ++problems;
        }
    }

    if (!quiet) {
        wprintf(L"\n");
        if (problems == 0) {
            wprintf(L"[OK] 卸载完成，没有残留。\n");
        } else {
            wprintf(L"[!] 卸载完成，但有 %d 项需要留意（见上方提示）。\n", problems);
        }
    }
    return problems == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
//  状态 / 残留扫描
// ---------------------------------------------------------------------------
static int DoStatus() {
    std::wstring installDir = GetInstallDir();

    wprintf(L"TaskbarEqWidth 状态\n");
    wprintf(L"-------------------------------------------\n");
    wprintf(L"DLL 是否驻留 explorer : %s\n", IsDllResident() ? L"是（正在生效）" : L"否");

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, TEQW_SHM_STATUS);
    if (hMap) {
        auto* st = static_cast<TeqwStatus*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0,
                                                          sizeof(TeqwStatus)));
        if (st && st->magic == TEQW_STATUS_MAGIC) {
            wprintf(L"钩子状态             : %s\n", st->hookOk ? L"已挂上" : L"未挂上");
            wprintf(L"当前按钮宽度         : %lu\n", st->itemWidth);
        }
        if (st) UnmapViewOfFile(st);
        CloseHandle(hMap);
    }

    DWORD w = RegGetDword(TEQW_REG_CFG_KEY, TEQW_REG_VAL_WIDTH, 0);
    wprintf(L"配置中的宽度         : %lu\n", w);
    wprintf(L"安装目录             : %s (%s)\n", installDir.c_str(),
            FileExists(installDir) ? L"存在" : L"不存在");

    HKEY hKey = nullptr;
    bool autostart = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TEQW_REG_RUN_KEY, 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS) {
        autostart = RegQueryValueExW(hKey, TEQW_REG_RUN_VALUE, nullptr, nullptr, nullptr,
                                     nullptr) == ERROR_SUCCESS;
        RegCloseKey(hKey);
    }
    wprintf(L"开机自启             : %s\n", autostart ? L"已启用" : L"未启用");
    return 0;
}

static int DoVerify() {
    std::wstring installDir = GetInstallDir();
    int found = 0;

    wprintf(L"残留扫描\n");
    wprintf(L"-------------------------------------------\n");

    if (FileExists(installDir)) {
        wprintf(L"[残留] 目录存在: %s\n", installDir.c_str());
        ++found;
    }
    if (IsDllResident()) {
        wprintf(L"[残留] DLL 仍驻留在 explorer.exe 中\n");
        ++found;
    }
    if (RegGetDword(TEQW_REG_CFG_KEY, TEQW_REG_VAL_MANAGED, 0xFFFFFFFF) != 0xFFFFFFFF) {
        wprintf(L"[残留] 注册表键存在: HKCU\\%s\n", TEQW_REG_CFG_KEY);
        ++found;
    }
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TEQW_REG_RUN_KEY, 0, KEY_READ, &hKey) ==
        ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, TEQW_REG_RUN_VALUE, nullptr, nullptr, nullptr, nullptr) ==
            ERROR_SUCCESS) {
            wprintf(L"[残留] 开机自启项存在: HKCU\\%s\\%s\n", TEQW_REG_RUN_KEY,
                    TEQW_REG_RUN_VALUE);
            ++found;
        }
        RegCloseKey(hKey);
    }

    if (found == 0) {
        wprintf(L"未发现任何残留，系统干净。\n");
    } else {
        wprintf(L"\n共 %d 项。执行 --uninstall 可一键清除。\n", found);
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    DWORD width = TEQW_DEFAULT_WIDTH;
    bool autostart = false;
    bool quiet = false;
    bool uninstall = false;
    bool keepFiles = false;
    bool status = false;
    bool verify = false;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (_wcsicmp(a, L"--uninstall") == 0 || _wcsicmp(a, L"-u") == 0) {
            uninstall = true;
        } else if (_wcsicmp(a, L"--keep-files") == 0) {
            keepFiles = true;
        } else if (_wcsicmp(a, L"--status") == 0) {
            status = true;
        } else if (_wcsicmp(a, L"--verify") == 0) {
            verify = true;
        } else if (_wcsicmp(a, L"--autostart") == 0) {
            autostart = true;
        } else if (_wcsicmp(a, L"--quiet") == 0) {
            quiet = true;
        } else if (_wcsicmp(a, L"--width") == 0 && i + 1 < argc) {
            width = static_cast<DWORD>(_wtoi(argv[++i]));
        } else if (_wcsicmp(a, L"--help") == 0 || _wcsicmp(a, L"-h") == 0) {
            wprintf(L"TaskbarEqWidth —— 让 Win11 任务栏按钮等宽（保留窗口标题）\n\n");
            wprintf(L"  TaskbarEqWidth.exe                 安装并立即生效\n");
            wprintf(L"  TaskbarEqWidth.exe --width 200     指定按钮宽度（50-400）\n");
            wprintf(L"  TaskbarEqWidth.exe --autostart     同时注册开机自启\n");
            wprintf(L"  TaskbarEqWidth.exe --status        查看状态\n");
            wprintf(L"  TaskbarEqWidth.exe --verify        扫描残留\n");
            wprintf(L"  TaskbarEqWidth.exe --uninstall     完全卸载\n");
            wprintf(L"  TaskbarEqWidth.exe --uninstall --keep-files   卸载但保留文件\n");
            return 0;
        }
    }

    if (quiet) FreeConsole();  // 开机自启时不弹黑框

    if (uninstall) return DoUninstall(keepFiles, quiet);
    if (status) return DoStatus();
    if (verify) return DoVerify();
    return DoInstall(width, autostart, quiet);
}
