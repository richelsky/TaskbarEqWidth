// ============================================================================
//  shared.h  —— TaskbarEqWidth 公共定义
//  作用：管理器(TaskbarEqWidth.exe) 与 注入DLL(TaskbarEqWidthHook.dll) 共用。
//
//  设计原则（这是"干净卸载"的基础）：
//    本工具与系统交互的全部入口，都集中定义在这个文件里。
//    只要能数清这里有几个名字，就能数清它在你电脑上留下了什么。
// ============================================================================
#pragma once

#include <windows.h>

// ---- 身份标识 --------------------------------------------------------------
#define TEQW_APP_NAME          L"TaskbarEqWidth"
#define TEQW_HOOK_DLL_NAME     L"TaskbarEqWidthHook.dll"
#define TEQW_MANAGER_EXE_NAME  L"TaskbarEqWidth.exe"

// ---- 内核对象（进程退出/句柄关闭即自动释放，不落盘、不留痕）-----------------
#define TEQW_EV_UNLOAD         L"Local\\TaskbarEqWidth_Unload"    // 管理器 -> DLL：请卸载
#define TEQW_EV_UNLOADED       L"Local\\TaskbarEqWidth_Unloaded"  // DLL -> 管理器：已卸载
#define TEQW_EV_INITDONE       L"Local\\TaskbarEqWidth_InitDone"  // DLL -> 管理器：已就绪
#define TEQW_MSG_WINDOW_CLASS  L"TaskbarEqWidth_MsgHost"          // DLL 的消息窗口类名
#define TEQW_MUTEX_NAME        L"Local\\TaskbarEqWidth_Mutex"

// ---- 注册表位置（全部在 HKCU 下，卸载时逐项精确删除）----------------------
// 仅此两处。不碰 HKLM、不装服务、不写计划任务、不动系统目录。
#define TEQW_REG_RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define TEQW_REG_RUN_VALUE     L"TaskbarEqWidth"     // 开机自启（可选，可关）
#define TEQW_REG_CFG_KEY       L"Software\\TaskbarEqWidth"
#define TEQW_REG_VAL_WIDTH     L"ItemWidth"          // 按钮宽度上限，DWORD
#define TEQW_REG_VAL_RESERVED  L"ReservedWidth"      // 任务栏右侧预留空白，DWORD
#define TEQW_REG_VAL_MANAGED   L"Managed"            // 标记：由管理器安装（DWORD）

// ---- 运行状态（共享内存，explorer 退出即自动消失，不落盘）-------------------
#define TEQW_STATUS_MAGIC      0x54514557u  // 'TEQW'
#define TEQW_SHM_STATUS        L"Local\\TaskbarEqWidth_Status"
struct TeqwStatus {
    DWORD magic;        // == TEQW_STATUS_MAGIC 才视为有效
    DWORD hookOk;       // 1 = 挂钩成功
    DWORD itemWidth;    // 当前实际生效的按钮宽度
    DWORD fromCache;    // 1 = 本次未联网，用的是缓存 PDB
    DWORD initDone;     // 1 = 初始化流程已走完（无论成功还是失败）
    DWORD downloadPct;  // PDB 下载进度 0..100；0xFFFFFFFF = 尚未开始下载
    // ---- 布局几何（供 --status 显示，也用于诊断"留不出空白"这类问题）----
    DWORD buttonCount;  // 观测到的任务栏按钮数
    DWORD reserved;     // 当前生效的预留空白（DIP）
    DWORD availWidth;   // 标定出的任务栏按钮区可用总宽（DIP），0 = 尚未标定
};

// ---- 默认参数 --------------------------------------------------------------
// XAML 宽度单位是 DIP，默认 176 与系统"自适应宽度"上限一致，视觉最自然。
#define TEQW_DEFAULT_WIDTH     176
#define TEQW_MIN_WIDTH         50
#define TEQW_MAX_WIDTH         400
// 任务栏最右侧要留出的空白（DIP）。留白是必须的：任务栏按钮铺满整条任务栏时，
// 鼠标没有任何"空白处"可以右键，也就点不出「任务栏设置」。
#define TEQW_DEFAULT_RESERVED  120
#define TEQW_MIN_RESERVED      0
#define TEQW_MAX_RESERVED      600

// ---- 管理器命令行 ----------------------------------------------------------
//   TaskbarEqWidth.exe                安装并立即生效（默认）
//   TaskbarEqWidth.exe --width 200    安装，并指定按钮宽度
//   TaskbarEqWidth.exe --autostart    额外注册开机自启
//   TaskbarEqWidth.exe --status       查看当前状态
//   TaskbarEqWidth.exe --verify       扫描是否有残留
//   TaskbarEqWidth.exe --uninstall    完全卸载（干净、彻底）
