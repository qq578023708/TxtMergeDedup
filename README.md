# TxtMergeDedup

**把目录下多个 txt 合并成一个文件并按行去重 —— 内存恒定，10GB 级数据也不会爆内存。**
*Merge multiple .txt files into one and remove duplicate lines — constant memory, handles 10GB+ datasets.*

[简体中文](#简体中文) | [English](#english)

![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D4)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C)
![Toolset](https://img.shields.io/badge/MSVC-v143-5C2D91)
![License](https://img.shields.io/badge/license-MIT-green)

---

## 简体中文

### 特性

- **内存恒定**：采用哈希分片外部去重，内存峰值约 100~300MB，与数据总量无关（30MB 和 100GB 占用相同量级）
- **高速**：大缓冲流式 I/O（8MB）+ 64 位哈希指纹；实测 5.99GB / 5.33 亿行耗时 216 秒
- **Win32 图形界面**：目录选择、**拖放文件夹**、进度条、实时状态、日志、中途取消
- **命令行模式**：方便脚本化、定时任务
- **健壮的行处理**：自动剥离 UTF-8 BOM、兼容 CRLF/LF 混合、正确处理文件末行无换行符、超长行跨读缓冲自动拼接
- **可选规范化**：丢弃空行、去除行尾空白后再比较
- **自动清理**：临时分片写在输出文件同目录，结束后自动删除

### 快速开始

**方式 A：使用预编译的 exe**

编译好的 `TxtMergeDedup.exe`（64 位，无外部依赖）可放在 Releases 或本地构建产物中获取，双击后：

选择源目录（或把文件夹拖进窗口）→ 确认输出路径 → 点「开始合并去重」。

**方式 B：从源码构建**

需要 Visual Studio 2022（含「使用 C++ 的桌面开发」工作负载）：

```text
双击打开 TxtMergeDedup.sln → 选择 Release | x64 → Ctrl+Shift+B
产物: bin\x64\Release\TxtMergeDedup.exe
```

也可以不开 IDE，直接用脚本构建：

```bat
build.bat
```

### 使用方法

#### GUI 模式

无参数启动 `TxtMergeDedup.exe` 即为 GUI。

| 控件 | 说明 |
|---|---|
| 源目录 | 要合并的目录，支持拖放；默认递归子目录 |
| 输出文件 | 默认 `<源目录>\merged_dedup.txt` |
| 包含子目录 | 是否递归扫描子文件夹 |
| 丢弃空行 | 空行完全不写入结果 |
| 去除行尾空白 | 先裁掉行尾空格/制表符，再参与去重比较 |

#### 命令行模式

```bat
TxtMergeDedup.exe <源目录> <输出文件> [-skipempty] [-trim] [-norecurse]
```

| 参数 | 说明 |
|---|---|
| `<源目录>` | 输入目录（必填） |
| `<输出文件>` | 输出 txt 路径（必填） |
| `-skipempty` | 丢弃空行 |
| `-trim` | 去除行尾空格/制表符 |
| `-norecurse` | 只处理顶层目录（默认递归） |

退出码：`0` 成功，`1` 失败或被取消。

```bat
:: 示例：合并 D:\logs 下所有 txt，去掉空行和行尾空白
TxtMergeDedup.exe D:\logs D:\out\merged.txt -skipempty -trim
```

### 工作原理

```text
阶段 1 —— 分片（流式读取，8MB 读缓冲）
  所有文件 → 逐行读取 → 计算 64 位哈希 → 按 hash % N 写入 N 个临时分片
  N = 总字节数 / 64MB（上限 256）→ 每个分片约 64MB

阶段 2 —— 去重 + 拼接
  逐个分片读入内存 → unordered_set 去重 → 顺序追加到输出文件
  单分片内存有上界，因此总内存与数据量无关
```

- 哈希：FNV-1a 64 + Murmur3 finalizer，用于快速指纹比较
- 输出顺序：**无序**（最快，省掉一遍排序 I/O）；重复行保留首次出现的那一条
- 临时文件：`<输出目录>\__dedup_tmp__\s000.tmp ...`，成功或取消结束都会清理

### 性能实测

| 数据集 | 行数 | 去重后 | 移除重复 | 耗时 |
|---|---|---|---|---|
| 30MB（含中文 / BOM / CRLF / 超长行） | 2,001,011 | 2,000,007 | 1,004 | ~1 秒 |
| 5.99GB / 18 个文件 | 532,845,581 | 256,885,934 | 275,959,647 | 216 秒 |

测试机为 NVMe/SSD 环境，瓶颈在磁盘 I/O（总 I/O 约 15GB：读 6GB + 写 6GB 分片 + 写 2.6GB 结果）。

### 已知限制

- **去重基于哈希指纹**（未做原文逐字节复核）：5 亿行规模下碰撞概率约 0.7%，常规数据可忽略；如需绝对精确，可改为「分片内哈希 → 原文列表复核」模式（内存约翻倍）
- **输入编码应统一**（UTF-8 或 GBK）：程序按字节比较，同一内容的不同编码不会被合并；不做转码
- 文件中出现**超长单行**（如几百 MB 无换行）时内存会随该行增长，属极端情况
- 仅提供 x64 构建

### 项目结构

```text
TxtMergeDedup/
├─ TxtMergeDedup.sln            Visual Studio 2022 解决方案
├─ TxtMergeDedup.vcxproj        VS 工程（Debug/Release，x64，v143，C++17）
├─ TxtMergeDedup.vcxproj.filters
├─ TxtMergeDedup.cpp            全部源码（核心算法 + Win32 GUI + CLI 入口）
├─ build.bat                    一键构建脚本（无需打开 IDE）
├─ README.md
└─ LICENSE
```

### 自检

源码内置 `SELFTEST` 编译开关，用于验证 GUI 消息链路：

```bat
cl /DSELFTEST /O2 /std:c++17 /EHsc /utf-8 TxtMergeDedup.cpp /Fe:selftest.exe /link /SUBSYSTEM:WINDOWS
selftest.exe
:: 退出码 0 表示 WM_APP_* 消息处理器全部正常
```

### 许可证

[MIT](LICENSE)

---

## English

### Features

- **Constant memory**: hash-partitioned external dedup keeps peak memory at ~100–300 MB regardless of dataset size (30 MB and 100 GB use the same order of magnitude)
- **Fast**: buffered streaming I/O (8 MB) + 64-bit hash fingerprints; 5.99 GB / 533 M lines measured in 216 s
- **Win32 GUI**: folder picker, **drag & drop**, progress bar, live status, log pane, cancel mid-run
- **Command-line mode**: easy to script or schedule
- **Robust line handling**: strips UTF-8 BOM, tolerates mixed CRLF/LF, handles files without a trailing newline, and joins extremely long lines that span read buffers
- **Optional normalization**: drop empty lines, trim trailing whitespace before comparison
- **Automatic cleanup**: temp shards are written next to the output file and removed afterwards

### Quick Start

**Option A — use the prebuilt binary**

A compiled `TxtMergeDedup.exe` (x64, no external dependencies) is available from Releases or as a local build artifact. Launch it, then:

Pick (or drag in) the source folder → confirm the output path → click **开始合并去重** (Start).

**Option B — build from source**

Requires Visual Studio 2022 with the "Desktop development with C++" workload:

```text
Open TxtMergeDedup.sln → select Release | x64 → Ctrl+Shift+B
Output: bin\x64\Release\TxtMergeDedup.exe
```

Or build from a shell without opening the IDE:

```bat
build.bat
```

### Usage

#### GUI

Launch `TxtMergeDedup.exe` with no arguments.

| Control | Meaning |
|---|---|
| Source folder | Directory to merge; drag & drop supported; recursive by default |
| Output file | Defaults to `<source>\merged_dedup.txt` |
| Include subfolders | Recurse into nested directories |
| Drop empty lines | Empty lines are never written to the output |
| Trim trailing whitespace | Trim trailing spaces/tabs before dedup comparison |

#### CLI

```bat
TxtMergeDedup.exe <source_dir> <output_file> [-skipempty] [-trim] [-norecurse]
```

| Flag | Meaning |
|---|---|
| `<source_dir>` | Input directory (required) |
| `<output_file>` | Output .txt path (required) |
| `-skipempty` | Drop empty lines |
| `-trim` | Trim trailing spaces/tabs |
| `-norecurse` | Top-level directory only (recursive by default) |

Exit code: `0` on success, `1` on failure or cancellation.

```bat
:: Merge every .txt under D:\logs, dropping blank lines and trailing spaces
TxtMergeDedup.exe D:\logs D:\out\merged.txt -skipempty -trim
```

### How It Works

```text
Pass 1 — partition (streaming, 8 MB read buffer)
  All files → read line by line → 64-bit hash → write to shard (hash % N)
  N = total_bytes / 64MB (capped at 256) → ~64MB per shard

Pass 2 — dedup + concatenate
  Load one shard at a time → unordered_set dedup → append to the output
  Per-shard memory is bounded, so total memory is independent of data size
```

- Hashing: FNV-1a 64 + Murmur3 finalizer (fast fingerprint comparison)
- Output order: **unordered** (fastest; avoids a sorting pass). The first occurrence of a duplicate is kept
- Temp files: `<output_dir>\__dedup_tmp__\s000.tmp ...`, cleaned up on success or cancellation

### Measured Performance

| Dataset | Lines | Unique | Removed | Time |
|---|---|---|---|---|
| 30 MB (Chinese text, BOM, CRLF, long lines) | 2,001,011 | 2,000,007 | 1,004 | ~1 s |
| 5.99 GB / 18 files | 532,845,581 | 256,885,934 | 275,959,647 | 216 s |

Measured on an NVMe/SSD setup; disk I/O is the bottleneck (~15 GB total: 6 GB read + 6 GB shards + 2.6 GB output).

### Limitations

- **Dedup relies on hash fingerprints only** (no byte-for-byte verification): collision probability is ~0.7% at 500 M lines — negligible for typical data. An exact mode (hash → original strings within a shard) is possible at roughly double the memory
- **Keep input encodings consistent** (UTF-8 or GBK): comparison is byte-based, so identical text in different encodings will not be merged. No transcoding is performed
- **Very long single lines** (hundreds of MB without a newline) increase memory proportionally — an edge case
- x64 builds only

### Project Layout

```text
TxtMergeDedup/
├─ TxtMergeDedup.sln            Visual Studio 2022 solution
├─ TxtMergeDedup.vcxproj        VS project (Debug/Release, x64, v143, C++17)
├─ TxtMergeDedup.vcxproj.filters
├─ TxtMergeDedup.cpp            All source (core algorithm + Win32 GUI + CLI entry)
├─ build.bat                    One-click build script (no IDE required)
├─ README.md
└─ LICENSE
```

### Self-Test

The source ships a `SELFTEST` build switch that exercises the GUI message pipeline:

```bat
cl /DSELFTEST /O2 /std:c++17 /EHsc /utf-8 TxtMergeDedup.cpp /Fe:selftest.exe /link /SUBSYSTEM:WINDOWS
selftest.exe
:: Exit code 0 means all WM_APP_* handlers ran without crashing
```

### License

[MIT](LICENSE)
