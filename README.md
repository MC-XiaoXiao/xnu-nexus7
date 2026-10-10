# XNU minimal platform for Nexus 7 (2012)

本副本增加 `TEGRA30_NEXUS7`，面向 Tegra30 / Cortex-A9 的 Nexus 7 (2012，grouper/tilapia)。在本机 Linux 上已完成全量 ARMv7 Mach-O 内核构建；尚未进行实机启动验证。

源代码基于 [HTC-Leo-Revival-Project/xnu](https://github.com/HTC-Leo-Revival-Project/xnu) 的 `d5c85d9b54bc27c338e69ff75dce2386dcfd723a` 及副本中已有的 HD2 修改。`xnu/config/MasterVersion` 是 **13.0.0**；目录名中的 iOS4 不会改变此代码基线，当前产物不是 Darwin 11 / iOS 4 内核。原 HD2 平台保留，其硬件路径按板型条件隔离。

## 最小平台范围

- 单核启动和 Cortex-A9 配置；UART 轮询输出、输入及虚拟地址切换期间的日志缓冲。
- ARM GIC + Tegra legacy interrupt controller，启用 TIMER1 IRQ32。
- 100 Hz 周期时钟中断，1 MHz 微秒计数器的软件 64 位扩展及 XNU 时间换算。
- 静态内核构建；关闭动态 KEXT 加载/卸载，避免继承 HD2 的内嵌 KEXT 包。

实现位于 `xnu/pexpert/arm/pe_tegra30.c`。默认 UARTD `0x70006300`；可通过编译宏 `NEXUS7_UART_PHYS` 改为 UARTA `0x70006000`，例如在重新全量构建前修改该文件的默认值。仅修改宏不会配置 UART pinmux 或波特率。存储、显示、触摸、USB、SMP、电源管理及 Android 引导封装均未实现；当前内核不能独立加载根文件系统。

硬件地址和路由依据工作目录内的资料：

- `../nexus7_2012_research/01_hardware/device_tree/tegra30.dtsi`
- `../nexus7_2012_research/02_source/legacy_board/board-grouper.c`
- `../nexus7_2012_research/03_datasheets/` 及 `04_boot_debug/`

定时器/LIC 寄存器语义还参考 Linux 的 [Tegra timer](https://github.com/torvalds/linux/blob/v6.12/drivers/clocksource/timer-tegra.c) 和 [Tegra IRQ controller](https://github.com/torvalds/linux/blob/v6.12/drivers/irqchip/irq-tegra.c)。

## 本机编译

从本仓库根目录执行：

```sh
./tools/bootstrap-host-mig.sh
./tools/build-nexus7.sh
python3 tools/tests/test-tegra30.py
python3 tools/check_source_completeness.py
```

已验证环境：Linux x86_64、Clang 23.1.1、GNU make、Python 3、Perl、flex、bison，外部 cctools ld64-956.6（支持 ARMv7 Mach-O）、tcsh、unifdef。无 Apple SDK。脚本使用仓库中的 ARM CRT，编译生成 MIG/configuration/export headers；不会依赖旧内核对象文件。

默认复用本机以下工具/源码，未把其二进制或头文件复制进源仓库：

| 环境变量 | 默认位置 / 内容 |
| --- | --- |
| `NEXUS7_DEPS` | `/home/xxiao/Projects/Project_iOS2ARM/build`，含 `toolchain/bin/arm-apple-darwin11-*`、`deps/tcsh/usr/bin/tcsh`、`deps/unifdef/usr/bin/unifdef` |
| `NEXUS7_CCTOOLS` | `$NEXUS7_DEPS/toolchain/bin` |
| `NEXUS7_MIG_REFERENCE` | `/home/xxiao/Projects/Project_iLEmu` |
| `NEXUS7_MIG_SOURCE` | `$NEXUS7_MIG_REFERENCE/sources/bootstrap_cmds-93/migcom.tproj`（本机版本 commit `1a46aba792cead257e2d3df03c5d38d88e615927`） |
| `NEXUS7_MIG_SHIM` | `$NEXUS7_MIG_REFERENCE/tools/abi_reference/migcom/shim`，MIG93 的 ILP32 主机头适配，保留其 MPL-2.0 声明 |
| `NEXUS7_MIG` | 本仓库 `build/host-mig`，已构建的兼容 MIG93 也可放在这里 |
| `NEXUS7_OBJROOT` | 本仓库 `build/obj-nexus7` |
| `JOBS` | `8` |

工具路径可通过以上变量迁移。MIG122 会生成此旧 XNU 不提供的接口，因此采用 MIG93。`tools/linux/` 包含 Clang/ld 参数转换、Mach-O section segment 修改和精确的 LLVM intrinsic 符号别名处理。历史源代码仍有编译警告，构建并非 warning-clean；脚本检查实际链接结果及子 make 错误。KPI symbol-set 包不在此最小静态内核构建范围内。

需要再次全量编译时，选择新的对象目录即可保留原构建用于比较：

```sh
NEXUS7_OBJROOT="$PWD/build/obj-fresh" ./tools/build-nexus7.sh
```

## 构建产物与验证

`build/output/` 下生成：

- `mach_kernel`：去除部分符号的内核。
- `mach_kernel.sys`：含符号的内核，供调试与加载器检查。
- `verification.json`：架构、入口、段布局、大小、SHA-256 和链接检查。
- `SHA256SUMS`：两个内核的校验和。

构建日志为 `build/build.log`。全量构建验证了 736 个输入对象均为 ARMv7 Mach-O，产物类型为 `MH_EXECUTE`，入口为 `0x800e08e0`，没有未解析符号，且包含 Nexus7 平台、入口、时钟和 VM 符号。SHA-256 以每次构建生成的报告为准；版本时间戳等元数据可导致重编译的哈希变化。

`tools/tests/test-tegra30.py` 使用实际平台 C 代码和模拟 MMIO，在主机验证 UART 超时/重映射、GIC/LIC 设置、计数器回绕、时钟中断分发、完整 EOI token 以及伪中断处理。它不能验证硬件时序或证明内核能在实机启动。

`SOURCE-MANIFEST.sha256` 只覆盖源文件；`build/` 和缓存不在其中。`SOURCE-AUDIT.md` 保留原 HD2 静态审计的历史结果，并追加本次 Nexus7 的构建证据。

## 后续加载器必须满足的约定

当前文件是 XNU Mach-O 内核，不能直接作为 Android boot.img 刷入。需要另行实现加载器：

1. 初始化 SDRAM，按 Mach-O `LC_SEGMENT` 的映射关系装载文件段并清零内存段尾部；依据 `LC_UNIXTHREAD` 入口跳转。链接虚拟地址起于 `0x80001000`，RAM 物理基址为 `0x80000000`；加载器须统一 `physBase`、`virtBase` 和实际装载地址。
2. CPU0 以 ARM 特权模式进入，IRQ/FIQ 关闭，其他 CPU 停驻；采用 MMU 关闭的入口路径。正确处理缓存和已存在映射。
3. 在 r0 提供本树 `xnu/pexpert/pexpert/arm/boot.h` 定义的 `boot_args`，包括有效内存边界、命令行及 XNU 所需的 Apple 扁平设备树。Linux DTB 的原始字节不能直接替代该设备树。
4. `topOfKernelData` 指向预留、16 KiB 对齐且已清零的 bootstrap 页表区，避免与内核、boot_args 和设备树重叠；其后的 VM 引导分配也必须有可用内存。
5. 预先初始化所选 UART 的 pinmux、时钟和 divisor，以及 `TIMERUS_USEC_CFG` 的 1 MHz 计数。微秒计数必须持续运行；当前时钟扩展要求至少每 `2^32` 微秒采样一次，不支持 suspend。

这些是后续 bring-up 的前提，尚未用实机验证。此阶段没有生成或刷写 boot.img。

## 许可证

既有文件继续适用各自许可声明，参见 `xnu/APPLE_LICENSE`、`xnu/LICENSE` 和 `LICENSE-NOTICE.md`。新增平台 C 文件带有 BSD-2-Clause SPDX 标记；未对没有声明的其他文件新增统一许可。研究资料、外部工具链、Apple 用户态、固件和引导镜像不随源代码分发。
