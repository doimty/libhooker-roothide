# libhooker roothide

配置好 roothide `THEOS` 和 `build-config.mk` 指定的 iOS SDK 后，在仓库根目录编译、打包并安装：

```sh
make do
```

默认构建发行版，包含 `arm64` 和 `arm64e`，最低系统版本由 `build-config.mk` 指定。通过 SSH 别名 `15` 安装，可用 `make do THEOS_DEVICE_IP=<地址或别名>` 指定设备。只打包用 `make package`，清理用 `make clean`，产物位于 `packages/`。

默认引擎为 `libjoker.dylib`，兼容入口链接到同一引擎。使用 `./mybuild.sh 1` 构建并安装默认引擎，或使用 `./mybuild.sh 2` 改为 `libhooker.dylib`；脚本会先清理旧产物。安装后从 bootstrap／越狱工具重启用户空间，使已有进程载入新版。

包信息位于 `control`，安装布局与维护脚本位于 `layout/`。

需要使用原上游构建方式时，执行 `make clean THEOS_PACKAGE_SCHEME=`，再执行 `make package THEOS_PACKAGE_SCHEME=`。

## iOS 15 兼容候选

本地候选将部署目标与固件依赖下限统一为 `15.0`，保留 iOS `17.3.1` SDK、`arm64` / `arm64e` 和 `< 18.0` 的依赖上限。这是构建及依赖范围，不代表该范围内的设备均已验收；新版 SDK 不要求部署目标与 SDK 版本相同。

保留现代 dyld 后端、镜像引用管理和 roothide 内存后端，不因旧文件或注释而切回上游的私有句柄解码。`_dyld_get_dlopen_image_header` 自 iOS 16 才提供，缺失时先按 dyld 已加载镜像的完整路径匹配，再对磁盘别名做规范化回退；共享缓存镜像没有独立磁盘文件时，不再因 `realpath` 失败而被拒绝。`_dyld_image_*` 属长期公开 API，不解析 dlopen 句柄位。`/usr/lib/TweakLoader.dylib` 与 `/usr/lib/TweakInject.dylib` 使用相对链接指向 `libhooker/libinjector.dylib`，接入 RootHide 的固定加载入口，而不是指向 Hook 引擎。

内存后端仍要求目标进程提供 `substrate_vm_protect` 和 `substrate_hook_memory`；这些接口缺失时会拒绝 Hook 提交，不用降低权限校验来掩盖缺失。dyld SPI、旧/新共享缓存符号表、实际 C / Objective-C Hook、插件加载和 SpringBoard 安全模式仍需在目标 iOS 15 环境验证。该候选仅面向 RootHide，不把降低版本号视为普通 rootless / rootful 适配。

## 代码结构

| 目录或文件 | 职责 |
| --- | --- |
| `libhooker/as-aarch64/`、`libhooker/disas-aarch64/` | AArch64 指令编码与重定位 |
| `libhooker/func/` | 函数入口 Hook、原调用跳板与短函数兼容 |
| `libhooker/mem/` | 内存后端、线程暂停、影子页事务、资源回滚和头部中继 |
| `libhooker/dyld/` | Mach-O 符号解析、镜像生命周期和共享缓存查询 |
| `libblackjack/objc/` | Objective-C 方法 Hook 与继承方法跳板 |
| `compat/` | Substrate 与 ElleKit 兼容接口 |
| `injector/`、`safemode/` | 插件筛选加载、崩溃保护与安全模式界面 |
| `roothide.mk`、`build-config.mk`、`layout/` | roothide 构建配置、安装布局和维护脚本 |
| `packaging/` | 原上游包信息与独立安装布局 |
| `tests/`、`scripts/` | 回归用例、包校验及设备测试工具 |

`roothide.mk` 按符号、重定位、内存、Objective-C 和兼容层分组维护实际源码清单。上游实现继续由根 `Makefile` 的非 roothide 分支管理。

## 本地验证

以下命令在仓库根目录执行：

```sh
python3 tests/test-ios15-packaging.py
bash scripts/test-host.sh
make package
python3 scripts/verify-package.py packages/org.coolstar.libhooker_1.6.9-22+roothide_iphoneos-arm64e.deb
bash scripts/build-device-rollback.sh
bash scripts/build-device-image-lookup.sh
```

宿主测试在 macOS 上使用 Xcode 命令行工具及 AddressSanitizer、UndefinedBehaviorSanitizer，覆盖指令编码、影子页、线程暂停、内存后端、Hook 资源、头部中继和符号缓存。包校验另需 `dpkg-deb`，检查元数据、相对链接、双架构、最低系统版本、引擎依赖及导出接口；更改包版本后应传入对应 DEB。校验 `libhooker` 名称的产物时，在命令末尾追加 `libhooker`。

`build-device-rollback.sh` 使用 `tests/device-rollback.entitlements.plist` 签名独立的双架构回滚测试程序，输出到 `worker/device-rollback/`，不连接或安装到设备。

本地测试与打包通过不代表设备验收完成。注入、安全模式和实际 Hook 行为仍需在目标设备上验证。构建中间文件位于 `.theos/`、`worker/`，发行包位于 `packages/`，均不纳入版本控制。

`1.6.9-22+roothide` 是 iOS 15 镜像查找修复候选，不代表完整 ElleKit ABI 兼容或真机重启验收完成。崩溃地址与断言的对应关系、宿主回归覆盖和尚未验证的项目见 [调查记录](docs/ios15-launchd-lookup.md)。`worker/lh-image-probe` 在独立进程中查询 IOSurface 与两个私有符号，不安装 Hook、不写目标内存、不操作 launchd，也不会自动安装或运行到设备上。
