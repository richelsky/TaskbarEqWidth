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
3. 挂钩它 —— 每次按钮布局时，按本机的实现路径调整按钮的 XAML 宽度
4. 于是所有按钮等宽，标题照常显示
```

第 3 步要分两条路走，因为 **Windows 有两套任务栏标签实现**：

| 本机情况 | IconPanel 的结构 | 等宽做法 |
|---|---|---|
| 带原生标签实现（较新版本） | **2 列 Grid**（图标列 + 标签列） | 标签列是 Auto 宽度，直接改列宽会被系统布局重算回去；改为往该列塞一个固定宽度的空 `Border` 把列"撑"住，并给标签文字设 `MaxWidth` 防止长标题撑开 |
| 旧式实现 | 普通面板 | 直接给它设 `Width` |

判据是运行时读 `IconPanel` 的列数，结论会写进 `hook.log`。只实现其中一条的话，
在另一类系统上会出现"钩子挂上了、日志说成功、任务栏毫无变化"——最难排查的失败。
这条思路与 Windhawk 的 `taskbar-labels` 模块一致（它用 `WindhawkLabelSpacer` 做同一件事）。

### 右侧留白（v0.2 新增，很重要）

等宽实现之后立刻出现一个新问题：**按钮铺满了整条任务栏**。
「从不合并」下的自适应布局会让按钮刚好铺到托盘边上，右侧一点空白都不剩 ——
鼠标没有地方可以右键，也就点不出「任务栏设置」。这不是等宽的副作用，是等宽之后
把"按钮之间的空隙"也吃掉了（原来是靠长短不一留下的缝）。

所以宽度是**动态算**出来的，而不是固定 176：

```
每个按钮宽 = min(可用总宽 / 按钮数, 上限 176) - 预留留白 / 按钮数
右侧留白   = 预留值（默认 120 DIP），改完立即生效、不需要重装
```

可用总宽从哪来？不猜、也不去翻 XAML 树，而是用**系统自己的判断**标定一次：
钩子跑在系统布局之后，此时按钮上写着的宽度如果**不等于我们刚写入的值**，
那它就是"可用总宽 / 按钮数"。这个判据天然避开了自我反馈的死循环。

如果发现外层宽度始终由系统掌控（我们写不住），程序会自动退让成"只约束内容宽度"
——效果一样，但不会变成每帧都在改的无限重排。一切结论都会写进 `hook.log`。

留白多少随时可调：

```
TaskbarEqWidth.exe --reserved 160      # 改完 1 秒内生效，不用重装
```


第 2 步是关键技术点。这个函数不导出，Windhawk 靠自带符号库找；本工具**运行时从微软公共符号
服务器下载对应的 PDB**（`msdl.microsoft.com`），再用 `dbghelp` 按名字定位。做法与开源项目
SlimBar11 一致。**因此首次安装需要联网下载 47.6 MB 的 PDB**，之后走本地缓存。

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

3. **首次运行需联网**下载 PDB。本机实测这个 PDB 有 **47.6 MB**，首次注入要等十几秒
   到一分钟（`--status` 会告诉你是"在线下载"还是"本地缓存"）。之后走缓存，秒开。
   离线环境装不上。

4. **关于"原生标签实现"**。Windhawk 那个模块里有个 `g_hasNativeLabelsImplementation`
   判断，来自 Windows 隐藏特性开关 `29785186`。也就是说近几版 Win11 的任务栏内部
   自带一套标签实现（设置里「从不」能显示标题，走的就是它），此时按钮的 `IconPanel`
   是一个 2 列 Grid。本工具会在运行时判断列数并分别处理两条路径（见上文表格），
   但**不接管标签文字的绘制**——标签仍由系统原生渲染。哪天原生实现把宽度也变成
   可配置，就不需要本工具了。

   副作用：原生标签路径下会在 XAML 树里留下一个 0×0 的空占位元素
   （`TaskbarEqWidthSpacer`）。卸载时它的宽度归零、不再影响布局，但要等 explorer
   重启才真正消失。这是刻意的——Windhawk 模块的注释里写明直接删这个元素会让
   运行指示条跑到按钮半透明底色后面。

5. **诊断入口是 `hook.log`**。钩子没挂上时，它自己会把卡在哪一步写进
   `<安装目录>\hook.log`（找不到 DLL / PDB 下不来 / 符号名变了 / MinHook 各步返回值）。
   `--status` 会直接把末尾几行打给你看。这是排查的唯一线索，卸载时会被一起删掉。

6. **尚未实机验证**（v0.1）。`--status` 的输出就是给你判断用的：显示"已挂上"才算成功。

7. **只测过 x64**。ARM64 需要另编译。

---

## 如果这东西让你不安

那就不用它。前面对比过的方案里，还有两条路：

- **改回「始终合并」**：零成本，按钮天然等宽，代价是看不到窗口标题（悬停缩略图仍可看）
- **Windhawk + taskbar-labels 模块**：成熟稳当，但安装占约 820 MB

本工具的价值只在于：把 820 MB 换成 2 MB，代价是自己承担版本脆弱性和杀软误报。
