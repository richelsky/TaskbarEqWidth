# TaskbarEqWidth

让 Windows 11 任务栏在「**从不合并 + 显示窗口标题**」模式下，所有按钮**宽度一致**。

这是那个 80 KB 的 Windhawk 模块的**独立迷你版**——不装 Windhawk、不需要 820 MB 的编译工具链，
只保留"等宽"这一条功能。

> ⚠️ **当前是 v0.1 第一版，代码写完但尚未在你的机器上编译验证过。**
> 请先看文末「已知限制」再决定要不要装。

---

## 它到底做了什么

Win11 的「从不合并」模式下，任务栏按钮宽度由**窗口标题文字长度**决定，所以长短不一。
系统没有开关能固定宽度，只能改任务栏的 UI 渲染逻辑。

本工具的做法：

```
1. 把 TaskbarEqWidthHook.dll 注入 explorer.exe
2. 在 Taskbar.View.dll 里找到私有的 TaskListButton::UpdateButtonPadding
3. 挂钩它 —— 每次按钮布局时，把按钮里 IconPanel 的宽度设成固定值
4. 于是所有按钮等宽，标题照常显示
```

第 2 步是关键技术点。这个函数不导出，Windhawk 靠自带符号库找；本工具**运行时从微软公共符号
服务器下载对应的 PDB**（`msdl.microsoft.com`），再用 `dbghelp` 按名字定位。做法与开源项目
SlimBar11 一致。**因此首次安装需要联网**（约几 MB），之后走本地缓存。

---

## 干净卸载：先把话说清楚

卸载的彻底程度，取决于安装时留下了什么。这里是**全部**：

| # | 落点 | 说明 | 卸载时 |
|---|---|---|---|
| 1 | `%LOCALAPPDATA%\TaskbarEqWidth\` | 2 个文件 + `symbols\` PDB 缓存 | 整个目录删除 |
| 2 | `HKCU\Software\TaskbarEqWidth` | 一个 DWORD：按钮宽度 | 删值 + 删键 |
| 3 | `HKCU\...\CurrentVersion\Run\TaskbarEqWidth` | 开机自启，**仅 `--autostart` 时写入** | 删值 |
| 4 | explorer.exe 内存里的 hook 代码 | 进程退出即消失 | 主动卸载 |

**没有的东西**（这是设计约束，不是"暂时没做"）：

- ❌ 不装服务、不装驱动
- ❌ 不写计划任务（这点和 SlimBar11 不同——它用计划任务做自启，卸载容易留尾巴）
- ❌ 不碰 `HKLM`、不碰系统目录、不改 `PATH`、不改文件关联
- ❌ 不修改 explorer.exe 的磁盘文件（只改它运行时的内存）

### 卸载流程（`--uninstall`）

```
[1/5] SetEvent 通知 DLL 卸载 -> 恢复原生宽度 -> 撤销 trampoline -> FreeLibraryAndExitThread
      若 8 秒内没确认，直接重启 explorer.exe 强制清除（保证零残骸）
[2/5] 删 HKCU Run 里的自启项
[3/5] 删 HKCU\Software\TaskbarEqWidth
[4/5] 删安装目录里的文件
[5/5] 扫描残留并报告；最后用一个脱离的 cmd 延时删除目录本身
```

### 一条安全底线

**它绝不删除你运行 exe 的那个文件夹。**

安装时，程序会把自己和 DLL 复制到 `%LOCALAPPDATA%\TaskbarEqWidth\`，之后所有操作只围绕这个
专属目录。哪怕你把 exe 放在桌面上运行，卸载也只删 `%LOCALAPPDATA%` 下那一个目录，桌面上
只多出一个你手动放进去的 exe——你自己删掉即可。

而且卸载函数里有硬校验：**目标路径不严格等于 `%LOCALAPPDATA%\TaskbarEqWidth` 就拒绝删任何文件。**

---

## 编译

> ### 推荐：云端编译，本机零安装
>
> 不想装 3–7 GB 工具链的话，直接看 **[云编译指南.md](云编译指南.md)**。
> 把仓库推到 GitHub，Actions 的免费 Windows runner 自带完整 MSVC + Windows SDK，
> 编完下载 2–3 MB 成品即可。本机一个字节的编译环境都不用装。
>
> 仓库已经初始化好了（含 `.github/workflows/build.yml`），照指南推一次就行。

本地编译则需要 Visual Studio 生成工具（勾选"**使用 C++ 的桌面开发**"）+
Windows SDK 10.0.22000+。MinHook 源码已随包附带，编译过程本身不联网。

```bat
build.bat
```

> **当前这台机器的状态：编不了。**
> 已检测：`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` 存在，但只有一个空壳——
> 没有 `cl.exe`、没有 `vcvars64.bat`，`Windows Kits\10` 下也没有 `Include` / `Lib`。
> 也就是说 **MSVC 编译器与 Windows SDK 都尚未安装**。
>
> 要编译，需要装（约 3–7 GB，一次性，装完可以卸载）：
>
> 1. [Visual Studio 生成工具](https://visualstudio.microsoft.com/zh-hans/downloads/) → 勾选「使用 C++ 的桌面开发」
>    这一项会自动带上 MSVC + Windows SDK，一次到位。
> 2. 装完运行 `build.bat` 即可。
>
> 装完编译出的是约 2–3 MB 的成品。**编译工具链属于开发环境，与最终工具的体积无关**，
> 用完随时可以在"Visual Studio Installer"里卸载。

**如果不想装这几个 GB** → 走上面的[云编译指南](云编译指南.md)，本机零安装。
唯一要守住的底线是：**别去拿网上别人编译好的、会注入 explorer 的 exe**。
自己推源码、让 GitHub 编，是你自己的代码你自己的产物，这条底线才守得住。

成功后产物在 `bin\`：

```
bin\TaskbarEqWidth.exe        管理器（安装/卸载/状态）
bin\TaskbarEqWidthHook.dll    注入到 explorer 的钩子
```

---

## 使用

**建议用管理员身份运行**（注入需要 SeDebugPrivilege）。

```bat
:: 安装并立即生效（默认宽度 176，与系统自适应上限一致，视觉最自然）
TaskbarEqWidth.exe

:: 指定宽度，50-400 之间
TaskbarEqWidth.exe --width 200

:: 同时注册开机自启
TaskbarEqWidth.exe --autostart

:: 查看状态
TaskbarEqWidth.exe --status

:: 扫描残留
TaskbarEqWidth.exe --verify

:: 完全卸载
TaskbarEqWidth.exe --uninstall

:: 卸载但保留文件（想自己再研究代码时用）
TaskbarEqWidth.exe --uninstall --keep-files
```

宽度不建议超过 220：任务栏项目太多时会开始挤压图标。改宽度就是重新运行一次
`TaskbarEqWidth.exe --width N`，程序会先卸载旧实例再重新注入。

---

## 项目结构

```
TaskbarEqWidth\
├── build.bat                  一键编译
├── README.md
├── src\
│   ├── shared.h               公共定义（事件名、注册表路径、状态结构）
│   ├── manager.cpp            管理器：安装 / 卸载 / 状态 / 残留扫描
│   ├── hook.cpp               注入 DLL：消息窗口、挂钩、干净卸载
│   └── symbols.h / symbols.cpp  PDB 下载 + 私有函数定位
└── third_party\minhook\       MinHook（MIT），钩子引擎
```

---

## 已知限制（请务必读完）

1. **版本脆弱性**。挂钩依赖 `Taskbar.View.dll` 的函数名。Windows 大版本更新可能改掉它，
   届时钩子会失效（`--status` 会显示"未挂上"）。恢复办法：`--uninstall`，等更新。
   Windhawk 的模块同样有这个毛病，只是作者更新更快。

2. **可能被杀软拦截**。"把 DLL 注入 explorer.exe"本身是敏感动作，火绒 / 360 /
   Windows Defender 大概率会拦。需要把安装目录加白名单。这不是代码有问题，
   是这个技术路线的固有代价。

3. **首次运行需联网**下载 PDB。离线环境装不上。

4. **尚未实机验证**（v0.1）。`--status` 的输出就是给你判断用的：显示"已挂上"才算成功。

5. **只测过 x64**。ARM64 需要另编译。

---

## 如果这东西让你不安

那就不用它。前面对比过的方案里，还有两条路：

- **改回「始终合并」**：零成本，按钮天然等宽，代价是看不到窗口标题（悬停缩略图仍可看）
- **Windhawk + taskbar-labels 模块**：成熟稳当，但安装占约 820 MB

本工具的价值只在于：把 820 MB 换成 2 MB，代价是自己承担版本脆弱性和杀软误报。
