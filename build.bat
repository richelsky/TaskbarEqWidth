@echo off
rem ===========================================================================
rem  build.bat - 一键编译 TaskbarEqWidth (x64)
rem
rem  依赖（需自行安装，均为免费工具）：
rem    - Visual Studio 生成工具（勾选"使用 C++ 的桌面开发"）
rem    - Windows SDK 10.0.22000 或更高（自带 C++/WinRT 头文件）
rem  MinHook 源码已随包附带在 third_party\minhook，编译无需联网。
rem
rem  注意：本文件刻意不在括号块里放中文。cmd.exe 按非 UTF-8 代码页解析批处理时，
rem        多字节汉字有可能吞掉紧跟其后的 & 或 )，导致命令被拆错、编译莫名失败。
rem        所以错误处理统一用 goto，中文只出现在独立的 echo 行上。
rem ===========================================================================
chcp 65001 >nul 2>nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo.
echo === [1/4] 定位 Visual Studio 编译环境 ===
set "VCVARS="

rem --- 首选 vswhere：能自动发现 2017/2019/2022/2026 任何版本 ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)

rem --- 兜底：常见安装位置（含 VS2026/18 的新版本号命名）---
if not defined VCVARS (
  for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) do (
    if exist %%P set "VCVARS=%%~P"
  )
)

if not defined VCVARS goto :err_novs

echo     使用: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 goto :err_vcvars

rem --- 定位 C++/WinRT 头文件：先按 vcvars 给的版本，找不到就扫描所有 SDK 版本 ---
set "SDKROOT=%WindowsSdkDir%Include\%WindowsSDKLibVersion%cppwinrt"
if exist "%SDKROOT%\winrt\Windows.Foundation.h" goto :sdk_ok
set "SDKROOT="
rem 注意 %%~D 而不是 %%D：for 会给变量带上引号，%~ 才能去掉
for /d %%D in ("%WindowsSdkDir%Include\*") do (
  if exist "%%~D\cppwinrt\winrt\Windows.Foundation.h" set "SDKROOT=%%~D\cppwinrt"
)
:sdk_ok
if not defined SDKROOT goto :err_nosdk

if not exist bin mkdir bin
if not exist obj mkdir obj
if not exist obj\mh mkdir obj\mh

rem MinHook 的 hook.c 会生成 hook.obj，与我们的 hook.cpp 同名，
rem 所以两批目标文件分开输出到 obj\mh\ 和 obj\，避免互相覆盖。
rem /utf-8 必加：源码是 UTF-8，若不声明，MSVC 会按本地代码页解析，
rem 结果不只是中文注释变乱码，L"中文" 这类宽字符串字面量会直接编错。
rem WINVER/_WIN32_WINNT 也要显式给：winhttp.h 里
rem WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 这类枚举被版本宏保护，不给就找不到。
rem 必须是 c++20 而不是 c++17：c++17 下 C++/WinRT 会去包含 <experimental/coroutine>，
rem 而新版 MSVC STL 已经把它标记为待移除（报 C2338/STL1011）。c++20 走 <coroutine>。
rem 那个 _SILENCE_... 宏是第二道保险，万一某版头文件仍走老路径也不至于编不过。
set "CFLAGS=/nologo /O2 /MT /DNDEBUG /DWIN32 /D_WINDOWS /W3 /utf-8"
set "CXXFLAGS=%CFLAGS% /std:c++20 /EHsc /DUNICODE /D_UNICODE /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00"
set "CXXFLAGS=%CXXFLAGS% /D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS"
rem 用 -I 而不是 /I，避免 set "..." 里再套引号；SDK 路径含空格，必须带引号
set "INC=-Isrc -Ithird_party\minhook\include -I"%SDKROOT%""

echo.
echo === [2/4] 编译 MinHook（第三方钩子库，MIT 许可）===
cl %CFLAGS% /c /Foobj\mh\ ^
   third_party\minhook\src\hook.c ^
   third_party\minhook\src\buffer.c ^
   third_party\minhook\src\trampoline.c ^
   third_party\minhook\src\hde\hde64.c
if errorlevel 1 goto :err_mh

echo.
echo === [3/4] 编译注入 DLL（TaskbarEqWidthHook.dll）===
cl %CXXFLAGS% %INC% /c /Foobj\ src\hook.cpp src\symbols.cpp
if errorlevel 1 goto :err_dllobj

link /nologo /DLL /OUT:bin\TaskbarEqWidthHook.dll /MACHINE:X64 ^
   obj\hook.obj obj\symbols.obj ^
   obj\mh\hook.obj obj\mh\buffer.obj obj\mh\trampoline.obj obj\mh\hde64.obj ^
   windowsapp.lib runtimeobject.lib dbghelp.lib winhttp.lib psapi.lib ole32.lib oleaut32.lib
if errorlevel 1 goto :err_dlllink

echo.
echo === [4/4] 编译管理器 EXE（TaskbarEqWidth.exe）===
cl %CXXFLAGS% %INC% /c /Foobj\ src\manager.cpp
if errorlevel 1 goto :err_exeobj

link /nologo /OUT:bin\TaskbarEqWidth.exe /MACHINE:X64 /SUBSYSTEM:CONSOLE ^
   obj\manager.obj shell32.lib advapi32.lib
if errorlevel 1 goto :err_exelink

echo.
echo ================================================
echo  编译完成，产物在 bin\ 目录：
dir /b bin
echo ================================================
echo.
echo  下一步：
echo    1. 以管理员身份打开命令提示符，cd 到 bin 目录
echo    2. TaskbarEqWidth.exe                 安装
echo    3. TaskbarEqWidth.exe --status        查看状态
echo    4. TaskbarEqWidth.exe --uninstall     完全卸载
echo.
if not defined TEQW_NO_PAUSE pause
exit /b 0

:err_novs
echo [x] 未找到 Visual Studio 的 vcvars64.bat
echo     请安装 "Visual Studio 2022 生成工具" 并勾选 "使用 C++ 的桌面开发"
echo     下载: https://visualstudio.microsoft.com/zh-hans/downloads/
goto :fail

:err_vcvars
echo [x] 初始化编译环境失败（vcvars64.bat 返回错误）
goto :fail

:err_nosdk
echo [x] 未找到 C++/WinRT 头文件: %SDKROOT%
echo     请确认已安装 Windows SDK 10.0.22000 或更高版本
goto :fail

:err_mh
echo [x] MinHook 编译失败
goto :fail

:err_dllobj
echo [x] 注入 DLL 编译失败
goto :fail

:err_dlllink
echo [x] 注入 DLL 链接失败
goto :fail

:err_exeobj
echo [x] 管理器编译失败
goto :fail

:err_exelink
echo [x] 管理器链接失败
goto :fail

:fail
echo.
echo 编译未完成。
if not defined TEQW_NO_PAUSE pause
exit /b 1
