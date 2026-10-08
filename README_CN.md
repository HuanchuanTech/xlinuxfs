# xlinuxfs — macOS 上的 Linux 文件系统(FSKit)

> 📖 English version: [English](README.md)

<a href="https://apps.apple.com/cn/app/xlinuxfs/id6785355678"><img src="https://tools.applemediaservices.com/api/badges/download-on-the-app-store/black/zh-cn?size=250x83" alt="在 App Store 下载 xlinuxfs" height="44"></a>

一个 macOS 应用 + **FSKit 文件系统扩展**,用于挂载 **ext2/3/4、XFS、Btrfs** 卷——无需内核
扩展,无需 macFUSE。它从 [xntfs](../xntfs) 项目里**可复用的 FSKit 脚手架**抽取而来,并移除了
NTFS 引擎(ntfs-3g)。

支持 Apple Silicon 和 Intel，最低系统版本为 macOS 15.4。macOS 27 增加了应用内
原始卷镜像挂载，以及可选择只读/读写模式的设备挂载。macOS 15 保留 Linux 卷的
系统自动挂载流程，不加入 NTFS 专用的重新挂载兼容脚本。
构建前提与实测范围见 [平台支持说明](docs/platform-support.md)。
元数据、文件生命周期、预分配等能力的设计与边界见 [文件系统能力说明](docs/filesystem-capabilities.md)。

> **状态:可用。** 应用部分(控制面板、检测、诊断、引导式命令行 附加/分离/挂载)与 **FSKit 扩展**
> 均已实现:`probeResource` 从超级块识别 ext2/3/4、XFS、Btrfs,核心卷操作(读写、创建、删除、重命名、
> 符号链接、属性)都由真 Linux 驱动处理。桥接层经命令行测试程序端到端验证;系统级挂载仍需描述文件
> 授权 FSKit 模块(应用内挂载还需 macOS 27 —— 见 [构建与签名](#构建与签名))。
>
> **引擎:用 LKL(Linux Kernel Library)跑真 Linux 内核**,编成原生 Mach-O 的 `liblkl.a`,
> 让 ext2/3/4 + XFS + Btrfs 由**内核原生驱动**处理(带日志、可读写),而不是重新实现。LKL 的
> macOS 移植与构建记录在 `vendor/lkl/tools/lkl/darwin/README.md`;宿主层(`lkl_host_ops` +
> 基于 `FSBlockDeviceResource` 的块后端,再 `lkl_sys_mount`/`lkl_sys_*`)就在扩展的 `bridge/` 里。

```
xlinuxfs.app  (SwiftUI,App 沙盒 —— 控制面板)
 ├─ AppModel ── DiskArbitrationMonitor   检测/列出 Linux 卷(仅检测)
 │           └─ MountService             引导式命令行 附加/分离/挂载
 └─ Contents/Extensions/lklfuse.appex     FSKit 模块 —— 进程内 LKL Linux 引擎
        lklfuse.swift            @main UnaryFileSystemExtension
        lklfuseFileSystem.swift  probe(超级块 magic)/ load / unload
        lklfuseVolume.swift      FSVolume 操作,经 lkfs_* 桥接
        lklfuseItem.swift        FSItem ↔ Linux inode 号
        bridge/                  基于进程内 LKL 内核(liblkl.a)的 lkfs_* C 桥接
```

## 已有 vs. 待办(TODO)

- **应用 target `xlinuxfs/`** —— 控制面板 UI、`DiskArbitrationMonitor`(按 Linux 文件系统 GPT GUID /
  内容提示检测 Linux 卷)、`MountService`(引导式命令行 附加/分离/挂载)、`ExtensionStatus` 诊断、
  设置、本地化。基本沿用 xntfs。
- **扩展 target `lklfuse/`** —— 基于进程内 LKL 内核的 FSKit `FSUnaryFileSystem` / `FSVolume` /
  `FSItem` 实现(`bridge/lkl_fskit.*`、`lkl_blk_ops.c`、`linux_device_fskit.m`):
  - `probeResource` 从超级块 magic 识别 ext2/3/4(`0xEF53`)、XFS(`XFSB`)、Btrfs(`_BHRfS_M`),
    并报告卷标 + 原生文件系统 UUID。
  - `loadResource` 以只读模式加载元数据；激活时再应用显式只读/读写选项，或 App Group 中的
    按场景设置。只读挂载跳过日志恢复。
  - `lklfuseVolume` 各操作经 `lkl_sys_*` 由 Linux 驱动处理。
- **`Info.plist` 的 `FSMediaTypes`** —— Linux 文件系统 GPT GUID `0FC63DAF-…-3D69D8477DE4`、一个
  `Linux` 内容提示、以及无分区表整盘。**请在真机上确认 ext 卷的确切 DA 内容提示**(MBR 盘可能报
  `Linux`/`0x83`)。

## 从源项目中排除的内容

所有与 ntfs-3g 相关的部分:`ntfs-3g` 子模块 + `libntfs-3g.a`、C/Obj-C 桥接
(`bridge/ntfs_fskit.{c,h}`、`ntfs_device_fskit.m`)、Swift↔C 桥接头、`build-libntfs.sh` 脚本和
`test_bridge.c`。扩展 target 里的相关构建设置(`HAVE_CONFIG_H`、ntfs-3g 头搜索路径、静态库链接、
Obj-C 桥接头)也已从 Xcode 工程里剔除。

## 构建与签名

先运行 `git submodule update --init --recursive` 初始化 `vendor/lkl`,并安装 upstream LLVM 19
(或通过 `CLANG` 指定编译器)。Xcode 预构建阶段会自动生成双架构库及对应头文件。
依赖归属、fork 发布和更新步骤见 [LKL 依赖说明](docs/lkl-dependency.md)。

打开 `xlinuxfs.xcodeproj`,构建 `xlinuxfs` scheme。target 使用 Xcode 的**同步文件夹组**,所以放进
`xlinuxfs/` 和 `lklfuse/` 的文件会被自动纳入。

扩展声明了受限权限 `com.apple.developer.fskit.fsmodule`(`lklfuse/lklfuse.entitlements`);macOS 只有
在该权限被描述文件授权后才会加载它(你的团队需为 App ID 开通 **FSKit File System Module**
能力)。在 **系统设置 → 通用 → 登录项与扩展 → 文件系统扩展** 中启用该模块。

## 注意事项

- 通过 `xcodebuild` 构建(应用 + 内嵌 `lklfuse.appex`);可在 Xcode 里打开 `xlinuxfs.xcodeproj`
  确认也能加载。macOS 27 之前无法在应用内做系统级挂载(FSKit Mounter 仅 27 提供),期间桥接层
  由命令行测试程序验证。
- 应用图标(`assets/xlinuxfs-icon.svg`,已光栅化进 `Assets.xcassets/AppIcon.appiconset`)是与 xntfs 同系列的「企鹅 + 硬盘」设计;若改 SVG,用 `rsvg-convert` 重新导出各尺寸即可。
- Bundle ID:应用 `com.huanchuan.xlinuxfs`,扩展 `com.huanchuan.xlinuxfs.lklfuse`;`FSShortName` =
  `xlinuxfs`。按需改成你自己的标识符。

## 许可证与隐私

本项目嵌入 Linux 内核，整体采用 **GNU General Public License, version 2**，
详见 [LICENSE](LICENSE)。数据处理说明见 [隐私政策](PRIVACY_CN.md)。
