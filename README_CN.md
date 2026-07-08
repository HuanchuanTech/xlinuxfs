# xlinuxfs — macOS 上的 Linux 文件系统(FSKit + LKL)

> 📖 English version: [English](README.md)

<a href="https://apps.apple.com/app/xlinuxfs/id6785355678"><img src="https://tools.applemediaservices.com/api/badges/download-on-the-app-store/black/zh-cn?size=250x83" alt="在 App Store 下载 xlinuxfs" height="44"></a>

一个 macOS 应用 + **FSKit 文件系统扩展**,使用 **真正的 Linux 内核**(通过
[LKL](https://github.com/lkl/linux),Linux Kernel Library)读写 **ext2/3/4、XFS、Btrfs** 卷。
无需内核扩展,无需 macFUSE。

`lklfuse` 应用扩展把真正的内核态 Linux 文件系统驱动(日志,以及整套 ext4 / XFS / Btrfs 代码)
——编译成原生 Mach-O 的 **`liblkl.a`**——套上一层很薄的 FSKit `FSVolume` 映射层。每个卷的文件
操作都经由 `lkfs_*` C 桥走 `lkl_sys_*` 系统调用,所以对磁盘格式的处理就是内核自身的,而非重新实现。

Linux 磁盘**由系统自动挂载到 `/Volumes`**(DiskArbitration 探测扩展注册的 `FSMediaTypes`——
GPT 的 *Linux filesystem data* 类型、MBR 的 `Linux`(0x83)提示、以及无分区表整盘——无需任何
应用进程,与内置的 exFAT/MSDOS 模块完全一样)。`xlinuxfs` 应用是一个**控制面板**:列出 Linux
磁盘与已附加的映像,提供手动挂载/卸载与只读重新挂载,并附加/分离磁盘映像。沙盒无法运行
`hdiutil`/`mount`,因此这些在应用无法直接执行时会以**可复制的终端命令**形式给出。

```
xlinuxfs.app  (SwiftUI,App 沙盒 —— 控制面板;不是后台代理)
 ├─ AppModel ── DiskArbitrationMonitor   检测/列出 Linux 磁盘(仅检测)
 │           └─ MountService             引导式命令行 附加 / 分离 / 挂载
 └─ Contents/Extensions/lklfuse.appex    FSKit 模块 —— 进程内 Linux 引擎(+ 自动挂载)
        lklfuse.swift            @main UnaryFileSystemExtension
        lklfuseFileSystem.swift  probe(超级块 magic)/ load / unload
        lklfuseVolume.swift      每个 FSVolume 操作 → lkfs_* 桥接
        bridge/lkl_fskit.c       lkfs_* 走 lkl_sys_*(mount、getattr、readdir、read、write、create…)
        bridge/lkl_blk_ops.c     由 FSKit 后端支撑的 virtio-blk
        bridge/linux_device_fskit.m  基于 FSBlockDeviceResource / 映像文件的块 I/O
        + liblkl.a(arm64;进程内 Linux 内核,静态链接)
```

## 状态

| 部件 | 状态 |
|-------|-------|
| `lklfuse` 扩展(经 LKL 的 ext/XFS/Btrfs 引擎) | ✅ 可编译;引擎静态链接;FSKit 协议实现完整 |
| `xlinuxfs` 应用(UI、监视器、挂载服务、设置) | ✅ 可编译 |
| 英文 + 简体中文本地化 | ✅ `Localizable.xcstrings`(en、zh-Hans) |
| 应用图标 | ✅ 已生成,完整 AppIcon 集 |
| 在 Mac 上端到端挂载 | ⛔ 需要为你的团队开通 FSKit 权限 —— 见下文 |

引擎本身已独立验证:进程内内核可启动并以读写方式挂载 ext2/3/4、以只读方式挂载 XFS 与 Btrfs;
挂载 / 枚举 / 读 / 写 / 创建 / 重命名 / 硬链接 + 符号链接均由 CLI 测试程序对真实镜像交叉核对。
只有系统*加载*扩展这一步,依赖与你的 Apple 开发者账号绑定的代码签名/描述文件。

## 功能

1. **本地化(英文 + 简体中文)** —— `xlinuxfs/Localizable.xcstrings`(String Catalog)。
   新增语言只需添加 `localizations` 条目。
2. **可移动 Linux 介质自动挂载** —— 由**系统**而非应用处理:扩展的 `Info.plist` 为 GPT 的
   *Linux filesystem data* 类型(`0FC63DAF-…`)、MBR 的 `Linux` 内容提示、以及无分区表整盘
   注册了 `FSMediaTypes`,于是 DiskArbitration 会用我们的模块把识别到的 ext/XFS/Btrfs 卷自动
   挂载到 `/Volumes`——全程无需应用进程。应用只负责为 UI *检测/列出*磁盘。
3. **按场景只读** —— 设置里有两个通过 App Group 与扩展共享的按场景默认值:**磁盘驱动器**默认
   读写,**磁盘映像**默认只读。只读挂载是真正零写入的(内核以 `MS_RDONLY` 挂载,ext 加
   `noload` / XFS 加 `norecovery`,且后端以 `O_RDONLY` 打开)。
4. **同时挂载多个磁盘** —— 设备按 BSD 名跟踪,各自独立挂载。
5. **手动挂载/卸载** —— 每个设备有 *Mount…* 和 *Eject*。macOS 27 上应用通过 `FSClient` 在
   应用内挂载;macOS 26 上沙盒应用无权挂载第三方 FSKit 卷,于是给出可复制的
   **`hdiutil mountvol /dev/diskXsY`** 命令(`diskutil mount` 对 Linux 文件系统无效)。
6. **磁盘映像** —— macOS 无法通过磁盘工具附加 Linux 磁盘映像,因此 *Add Disk Image…* 选择一个
   原始 ext/XFS/Btrfs 映像,并给出可复制的 **`hdiutil attach [-readonly] <映像>`** 命令(默认
   只读);附加后,该卷会像普通磁盘一样**自动挂载**。*Detach Image…* 给出 `hdiutil detach`。

## 引擎 —— 进程内的真 Linux 内核

`liblkl.a` 是通过 LKL 把 Linux 内核编译成的**原生 arm64 Mach-O 对象**。它在扩展内启动一次;
每个被挂载的卷把其后端作为 virtio-blk 设备接入,并用内核自带的 ext4 / XFS / Btrfs 驱动挂载,
所以日志与恢复都是真的。内核 → Mach-O 的 macOS 移植(启动 + 挂载修复)记录在配套 `lkl` 树的
`MACOS_PORT_NOTES.md` 里。

## 构建

打开 `xlinuxfs.xcodeproj`,构建 `xlinuxfs` scheme。target 使用 Xcode 的**同步文件夹组**,所以
放进 `xlinuxfs/` 和 `lklfuse/` 的文件会被自动纳入。

引擎归档 `libs/liblkl.a` **不入库**(它是构建产物);用 `scripts/build-liblkl.sh` 重新构建,
它从 LKL 树的 macOS 构建对象组装而成(内核对象来自配套 `lkl` 树的 LKL 混合构建)。**仅 arm64。**

## 配置签名(实际运行扩展所必需)

扩展声明了**受限**权限 `com.apple.developer.fskit.fsmodule`(`lklfuse/lklfuse.entitlements`)。
macOS(AMFI)只有在该权限被描述文件授权后才会加载它——该团队的 App ID 需要在 Apple 开发者门户
开通 **FSKit File System Module** 能力,且应用与扩展的 App ID 都需要 **App Group**
(`<TeamID>.group.com.huanchuan.xlinuxfs`),只读设置才能与扩展共享。开通后,在 **系统设置 →
通用 → 登录项与扩展 → 文件系统扩展** 中启用该模块。

## 沙盒与挂载

在 macOS 26.5 的沙盒构建上验证过:

- **自动挂载是主路径。** 系统通过扩展的 `FSMediaTypes` 把识别到的 Linux 卷以读写(或按你的设置
  只读)挂载到 `/Volumes`——无需应用进程、无需挂载权限。
- **macOS 26 上沙盒应用无法手动挂载第三方 FSKit 卷。** 手动挂载仅在 macOS 27 上是应用内
  (`FSClient`,只能 `/Volumes`);在此之前是可复制的 `hdiutil mountvol` 命令。
- **`diskutil mount` 对 Linux 文件系统无效**,而 `mount -F -t xlinuxfs …` 会被扩展的沙盒拒绝
  ——可用的命令行是 `hdiutil mountvol`(同样经由 DiskArbitration)。
- **无法挂载到自定义文件夹**(沙盒化的 FSKit 卷只能 `/Volumes`)。
- **应用从不 shell 出去。** 沙盒拦截 `Process`,所以 `hdiutil` 以可复制的终端命令交给用户运行。

## 许可证

`xlinuxfs` 内嵌了 Linux 内核及其 ext4 / XFS / Btrfs 驱动,它们是 **GPL-2.0** 的,所以整个项目
按 **GNU 通用公共许可证第 2 版** 分发(见 [`LICENSE`](LICENSE))。

## 目录结构

```
xlinuxfs/                   应用 target(SwiftUI)
  xlinuxfsApp.swift         @main App + Settings 场景
  ContentView.swift         设备/映像列表 + 详情;挂载 / 附加 / 分离操作
  MountSheet.swift          手动挂载(显示只读模式,跟随设置)
  CommandSheet.swift        附加/分离 sheet(AttachImageSheet:默认只读)
  CopyableCommand.swift     共享的「复制此终端命令」控件
  DiagnosticsView.swift     扩展状态诊断页
  SettingsView.swift        按场景只读偏好
  Model/LinuxDevice.swift
  Services/                 AppModel、AppSettings、DiskArbitrationMonitor、MountService、
                            ExtensionStatus
  Localizable.xcstrings     en + zh-Hans
  Assets.xcassets/AppIcon   生成的图标集
lklfuse/                    FSKit 扩展 target
  lklfuse*.swift            @main + FSUnaryFileSystem + FSVolume + FSItem
  bridge/                   lkl_fskit.{h,c}、lkl_blk_ops.c、linux_device_fskit.m、lkl-include/
  Info.plist                FSShortName=xlinuxfs、FSMediaTypes、块资源
  lklfuse.entitlements      com.apple.developer.fskit.fsmodule + App Group + 沙盒
libs/liblkl.a               进程内 Linux 内核(由 scripts/build-liblkl.sh 构建)
scripts/build-liblkl.sh     从 LKL 的 macOS 构建组装 liblkl.a
assets/xlinuxfs-icon.svg    图标源文件
```
