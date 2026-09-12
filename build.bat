@echo off
rem ===========================================================================
rem  build.bat —— 一键编译 TaskbarEqWidth（x64）
rem
rem  依赖（需自行安装，均为免费工具）：
rem    - Visual Studio 2022 生成工具（勾选"使用 C++ 的桌面开发"）
rem    - Windows SDK 10.0.22000 或更高（自带 C++/WinRT 头文件）
rem  MinHook 源码已随包附带在 third_party\minhook，编译无需联网。
rem ===========================================================================
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

if not defined VCVARS (
  echo [x] 未找到 Visual Studio 2022 的 vcvars64.bat
  echo     请安装 "Visual Studio 2022 生成工具" 并勾选 "使用 C++ 的桌面开发"
  echo     下载: https://visualstudio.microsoft.com/zh-hans/downloads/
  if not defined TEQW_NO_PAUSE pause
  exit /b 1
)
echo     使用: %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 ( echo [x] 初始化编译环境失败 & if not defined TEQW_NO_PAUSE pause & exit /b 1 )

set "SDKROOT=%WindowsSdkDir%Include\%WindowsSDKLibVersion%cppwinrt"
if not exist "%SDKROOT%\winrt\Windows.Foundation.h" (
  echo [x] 未找到 C++/WinRT 头文件: %SDKROOT%
  echo     请确认已安装 Windows SDK 10.0.22000 或更高版本
  if not defined TEQW_NO_PAUSE pause
  exit /b 1
)

if not exist bin mkdir bin
if not exist obj mkdir obj
if not exist obj\mh mkdir obj\mh

rem 说明：MinHook 的 hook.c 会生成 hook.obj，与我们的 hook.cpp 同名，
rem       所以两批目标文件分别输出到 obj\mh\ 和 obj\，避免互相覆盖。
set "CFLAGS=/nologo /O2 /MT /DNDEBUG /DWIN32 /D_WINDOWS /W3"
set "CXXFLAGS=%CFLAGS% /std:c++17 /EHsc /DUNICODE /D_UNICODE"
set "INC=/I"src" /I"third_party\minhook\include" /I"%SDKROOT%""

echo.
echo === [2/4] 编译 MinHook（第三方钩子库，MIT 许可）===
cl %CFLAGS% /c /Foobj\mh\ ^
   third_party\minhook\src\hook.c ^
   third_party\minhook\src\buffer.c ^
   third_party\minhook\src\trampoline.c ^
   third_party\minhook\src\hde\hde64.c
if errorlevel 1 ( echo [x] MinHook 编译失败 & if not defined TEQW_NO_PAUSE pause & exit /b 1 )

echo.
echo === [3/4] 编译注入 DLL（TaskbarEqWidthHook.dll）===
cl %CXXFLAGS% %INC% /c /Foobj\ src\hook.cpp src\symbols.cpp
if errorlevel 1 ( echo [x] DLL 编译失败 & if not defined TEQW_NO_PAUSE pause & exit /b 1 )

link /nologo /DLL /OUT:bin\TaskbarEqWidthHook.dll /MACHINE:X64 ^
   obj\hook.obj obj\symbols.obj ^
   obj\mh\hook.obj obj\mh\buffer.obj obj\mh\trampoline.obj obj\mh\hde64.obj ^
   windowsapp.lib runtimeobject.lib dbghelp.lib winhttp.lib psapi.lib ole32.lib oleaut32.lib
if errorlevel 1 ( echo [x] DLL 链接失败 & if not defined TEQW_NO_PAUSE pause & exit /b 1 )

echo.
echo === [4/4] 编译管理器 EXE（TaskbarEqWidth.exe）===
cl %CXXFLAGS% %INC% /c /Foobj\ src\manager.cpp
if errorlevel 1 ( echo [x] 管理器编译失败 & if not defined TEQW_NO_PAUSE pause & exit /b 1 )

link /nologo /OUT:bin\TaskbarEqWidth.exe /MACHINE:X64 /SUBSYSTEM:CONSOLE ^
   obj\manager.obj shell32.lib advapi32.lib
if errorlevel 1 ( echo [x] 管理器链接失败 & if not defined TEQW_NO_PAUSE pause & exit /b 1 )

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
