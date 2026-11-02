# 选择性移植 Dopamine 2 内存兼容修复

## 来源与范围

- 本地分支：`feature/configurator-roothide`；修改前 HEAD：`d397284b741e8230930809d71f787db2a894f393`。
- 原提交：[w2599/libhooker-roothide 25a8f01b](https://github.com/w2599/libhooker-roothide/commit/25a8f01b7627990c5b752fc669e1c16b5460d353)，新增 Dopamine 2 缺失服务导出时的本地内存后端。
- 仅将核心源码、构建清单和配套测试移入工作树。未合并上游分支、未创建提交、未推送，也未安装到设备。
- 跳过 `bfdd33366850e106daeb2b7ea802570cd8b19335` 的发布流程：原打包脚本、源图标/depiction 和忽略规则保持本地版本。
- 包元数据与 `LHVersion()` 同步为 `1.6.9-23+roothide.2`；保留 SDK `17.3.1`、部署目标 `15.0`、双架构和 `< 18.0` 的固件范围。

## 生产行为

[后端选择器](../libhooker/mem/writeMem-roothide.c)优先使用完整的 `substrate_vm_protect` / `substrate_hook_memory` 对。

| 导出状态 | 行为 |
| --- | --- |
| 两项均存在 | 保留服务后端 |
| 两项均缺失，本地准备成功 | 整体切换到本地 protect/patch 对，并缓存成功选择 |
| 只存在一项 | 本次提交失败，后续允许缺失项出现后重试；不混用本地与服务接口 |
| 两项均缺失，本地准备失败 | 本次提交失败，不缓存失败；后续仍可准备本地后端 |
| 本地后端已成功，服务后来出现 | 保持已选择的本地后端，不在进程中途切换 |

[本地准备器](../libhooker/mem/legacy-memory.c)在暂停线程前分配并填充独立页、刷新指令缓存、设置 RX，随后用函数指针认证与原子发布。失败候选及 CAS 败者释放其页面。写入只接受整页；既有提交、字节核对、原权限恢复与事务回滚逻辑继续使用。

[独立写入器](../libhooker/mem/legacy-syscalls.S)直接使用既有 Darwin 保护 trap，执行 COW、拷贝和原权限恢复，不调用可能位于被改写页的 libc 代码。

### 相对于原提交的最小补强

原 writer 在恢复权限失败后立即返回。如果返回点恰在目标页且该页仍是 NX，外层 C 补救尚未执行就会失去取指权限。本次把一次恢复重试前移到独立页内部，完成后才返回，并保留首次失败码，让上层继续按提交失败处理、触发回滚。

这是一项有界缓解，不是任意恢复失败的通用保证。首次恢复和独立页内重试均失败、目标仍 NX、返回点位于该页时，失效路径依然存在。此限制由模拟用例显式记录；未加入新的进程终止或信号处理策略。

## 保留的本地功能

- iOS 15 缺失 header SPI 的镜像查找、共享缓存镜像精确路径匹配、磁盘别名回退。
- 移除 dyld fork 生命周期 SPI 伪锁的启动修复。
- Configurator 的进程/插件策略、独立管理器包和原有配置协议。
- `TweakLoader` / `TweakInject` 到 injector 的相对加载链接及包替换规则。
- 镜像查找、shipped22 fork 负控、配置策略和加载器回归。

新增的自动兼容后端不实现管理器旧版 alternate-memory-backend 或 OldABI 开关。

## 回归入口

### macOS/Xcode

```sh
python3 -m pip install --target worker/host-test-deps unicorn==2.1.2
export PYTHONPATH="$PWD/worker/host-test-deps${PYTHONPATH:+:$PYTHONPATH}"
bash scripts/test-host.sh
python3 tests/test-ios15-packaging.py
make package
python3 scripts/verify-package.py packages/org.coolstar.libhooker_1.6.9-23+roothide.2_iphoneos-arm64e.deb
bash scripts/build-device-rollback.sh
bash scripts/build-device-compat.sh
```

[宿主入口](../scripts/test-host.sh)保留原有 ASan/UBSan 测试，追加：

- [生产选择器桩测试](../tests/memory-backend.c)：服务、双缺失、两种单缺失、准备失败后服务恢复、始终双缺失下准备重试、连续失败后重试、本地成功后服务出现仍固定本地。
- [真实准备源码桩测试](../tests/legacy-memory.c)：分配失败、RX 失败释放及重试、成功缓存、确定性重叠发布的 CAS 败者释放，以及输入/权限查询校验。重叠发布用例不是实际双线程调度测试。
- [ARM64 指令模拟](../tests/legacy-writer.py)：从实际 Mach-O 对象按符号范围提取 writer，确认无节重定位，使用 Unicorn 和 Mach trap 桩覆盖 4 KiB/16 KiB 页、原权限、首次保护失败、同页成功和瞬时恢复失败重试。持续恢复失败的 NX 返回是已知限制用例，而非恢复成功用例。

CI 安装固定版本的 Unicorn 并运行上述宿主测试，另外构建并保留 rollback/compat 诊断程序；不运行到设备。

### 设备诊断（后续验收）

[compat 工具](../tests/device-roothide-compat.c)在自身进程中加载指定引擎，测试 Mach-O 同页函数与共享缓存 XPC 接收 Hook/original/reply。[rollback 工具](../tests/device-rollback.c)新增 `legacy-self-page` 原样重写案例，并允许在缺失服务时用真实本地后端执行已有故障矩阵。

```sh
DeviceCompat-arm64e /absolute/path/to/libjoker.dylib
DeviceRollback-arm64e --case legacy-self-page
DeviceRollback-arm64e --list
```

这些程序仅由脚本构建，没有自动上传或执行。实际 iOS 15 调试权限、COW/W^X、PAC、目标进程替换启动和持续恢复失败策略仍需单独验收；宿主桩和 CPU 模拟不替代它们。

## 本轮验证记录

本轮在 Windows 上使用已有 Zig 0.14.1/Clang 19、缓存的 iPhoneOS 16.5 头文件和工作树内的 Unicorn 2.1.2 验证依赖。缓存 SDK 的扁平化 pthread 软链接以工作树内 include overlay 还原；缺失的 XPC 声明从 Zig 随附的 Apple 公共头文件补充。这些头文件仅用于对象级交叉编译检查，没有改变发行配置的 17.3.1 SDK，也不替代完整 Xcode SDK 的链接与设备验收。

本地验证由 [工作树验证脚本](../worker/validate-selective-port.py)生成 [结果报告](../worker/validation/report.json)和逐项日志，最终运行退出码为 0。

| 已运行项目 | 结果 |
| --- | --- |
| 生产后端选择与提交/回滚桩测试，8 个独立进程模式 | 全部通过，每模式约 55.8 万次断言 |
| 真实本地准备源码桩测试，4 个独立进程模式 | 全部通过；包括具体败者页释放、已发布页保留 |
| 核心 C 与两个设备诊断 C 的 arm64 / arm64e 对象编译，部署 15.0 | 8 个对象全部通过；核对了 Mach-O CPU subtype |
| 独立汇编 writer 的 arm64 / arm64e 对象构建与模拟 | 两架构各 6 项通过；持续同页恢复失败按已知失效用例记录 |
| 4 个 shell 脚本语法、源级打包契约、版本/固件下限一致性、差异空白检查 | 全部通过 |
| 本地 dyld、injector、manager、兼容层、布局及原发布脚本保持不变 | Git 差异核对通过 |

交叉编译不包含链接，Windows 接口桩不执行真实 Mach/PAC。完整 macOS ASan/UBSan、Theos 链接/最终 DEB 校验及设备执行在本机未运行；报告明确列出这些待验证项。
