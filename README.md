# xTdb - Industrial TSDB Storage Core (V1.6)

高性能工业时序数据库存储内核，采用 RAW Fixed-Block Chunks + Central Directory 架构。

## 项目状态

✅ **阶段1完成**：物理层与布局管理器
- AlignedIO（16KB 对齐 I/O）
- LayoutCalculator（偏移量计算）
- 22个单元测试全部通过

✅ **阶段2完成**：头部定义与状态机
- ContainerHeader / RawChunkHeader / BlockDirEntry 结构体定义
- StateMutator（Active-low 状态机，SSD 友好）
- 25个单元测试全部通过

🔄 **进行中**：阶段3 - 写入路径（WAL + BlockWriter）

## 核心特性

### 设计目标
- 📈 **高写入吞吐**：百万点/分钟级
- 💾 **顺序 IO 友好**：低写放大
- 🔒 **崩溃可恢复**：WAL + 物理头部验证
- 🛠️ **易调试维护**：支持脱库扫描/修复
- 🔄 **分层存储**：RAW → COMPACT（后续引入压缩）

### V1.6 关键设计

#### 物理单位
- **Extent**：16KB（最小分配/对齐单位）
- **Block Classes**：RAW_16K / RAW_64K / RAW_256K
- **Chunk Size**：推荐 256MB（可配置）

#### 架构特点
- **集中目录（Central Directory）**：Meta Region 存放 ChunkHeader + BlockDir
- **数据分离**：Data Block 仅存 records（无 header）
- **SSD 友好**：Active-low 状态位（只允许 1→0 写入）
- **按 BlockClass 分池**：每个 Container 固定一种 Block 尺寸

## 快速开始

### 环境要求
- **操作系统**：Linux
- **编译器**：GCC 14.2+ 或 Clang（支持 C++17）
- **依赖**：
  - CMake 3.14+
  - Google Test

### 编译与测试

```bash
# 构建并运行所有测试
./build.sh --test

# 仅构建
./build.sh

# 运行特定测试
cd build
./test_alignment
./test_layout
```

### 测试输出示例

```
Test project /home/admin/cxxproj/xTdb/build
    Start 1: AlignmentTest
1/4 Test #1: AlignmentTest ....................   Passed    0.12 sec
    Start 2: LayoutTest
2/4 Test #2: LayoutTest .......................   Passed    0.00 sec
    Start 3: StructSizeTest
3/4 Test #3: StructSizeTest ...................   Passed    0.00 sec
    Start 4: StateMachineTest
4/4 Test #4: StateMachineTest .................   Passed    0.01 sec

100% tests passed, 0 tests failed out of 4
Total Test time (real) =   0.13 sec
```

## API 使用示例

### 1. AlignedIO - 对齐 I/O 操作

```cpp
#include "xTdb/aligned_io.h"
using namespace xtdb;

// 创建 AlignedIO 实例
AlignedIO io;
io.open("/path/to/data.db", true, false);

// 分配 16KB 对齐的 buffer
AlignedBuffer buffer(kExtentSizeBytes);
buffer.zero();

// 写入（必须 16KB 对齐）
IOResult result = io.write(buffer.data(), kExtentSizeBytes, 0);
if (result != IOResult::SUCCESS) {
    std::cerr << io.getLastError() << std::endl;
}

// 预分配 256MB 空间
io.preallocate(256 * 1024 * 1024);
```

### 2. LayoutCalculator - 布局计算

```cpp
#include "xTdb/layout_calculator.h"
using namespace xtdb;

// 计算 RAW16K 布局（256MB chunk）
ChunkLayout layout = LayoutCalculator::calculateLayout(
    RawBlockClass::RAW_16K,
    kDefaultChunkSizeExtents);

std::cout << "Meta blocks: " << layout.meta_blocks << std::endl;
std::cout << "Data blocks: " << layout.data_blocks << std::endl;

// 计算物理偏移量
uint32_t chunk_id = 5;
uint32_t block_index = 100;
uint64_t offset = LayoutCalculator::calculateBlockOffset(
    chunk_id, block_index, layout);

std::cout << "Block offset: " << offset << " bytes" << std::endl;
```

### 3. StateMutator - 状态机操作

```cpp
#include "xTdb/state_mutator.h"
using namespace xtdb;

// 打开文件
AlignedIO io;
io.open("/path/to/data.db", true, false);

// 创建状态机
StateMutator mutator(&io);

// 初始化 chunk header
RawChunkHeaderV16 header;
header.chunk_id = 0;
header.chunk_size_extents = kDefaultChunkSizeExtents;
header.block_size_extents = getBlockSizeExtents(RawBlockClass::RAW_16K);
mutator.initChunkHeader(0, header);

// Chunk 生命周期操作
mutator.allocateChunk(0);                           // FREE → ALLOCATED
mutator.sealChunk(0, 1000000, 2000000, 0x12345678); // ALLOCATED → SEALED
mutator.deprecateChunk(0);                          // SEALED → DEPRECATED

// Block 操作
BlockDirEntryV16 entry;
entry.tag_id = 100;
entry.start_ts_us = 1000000;
mutator.initBlockDirEntry(128, entry);              // 初始化
mutator.sealBlock(128, 2000000, 1000, 0xABCDEF12);  // 封存
```

## 项目结构

```
xTdb/
├── include/xTdb/          # 头文件
│   ├── constants.h
│   ├── aligned_io.h
│   ├── layout_calculator.h
│   ├── struct_defs.h      # ✨ 阶段2：结构体定义
│   └── state_mutator.h    # ✨ 阶段2：状态机
├── src/                   # 源文件
│   ├── aligned_io.cpp
│   ├── layout_calculator.cpp
│   └── state_mutator.cpp  # ✨ 阶段2
├── tests/                 # 测试文件
│   ├── test_alignment.cpp
│   ├── test_layout.cpp
│   ├── test_struct_size.cpp    # ✨ 阶段2：T3
│   └── test_state_machine.cpp  # ✨ 阶段2：T4
├── docs/                  # 文档
│   ├── design.md          # V1.6 设计文档
│   ├── plan.md            # 实施计划
│   ├── phase1_summary.md  # 阶段1总结
│   └── phase2_summary.md  # ✨ 阶段2总结
├── build/                 # 构建输出（自动生成）
├── CMakeLists.txt         # CMake 配置
├── build.sh               # 构建脚本
└── README.md              # 本文档
```

## 测试覆盖

### T1-AlignmentCheck（10个测试）
- ✅ 有效对齐写入成功
- ✅ 未对齐 Buffer/Size/Offset 失败
- ✅ 多次对齐写入
- ✅ 对齐读取与验证
- ✅ Preallocate 对齐检查
- ✅ AlignedBuffer 自动对齐

### T2-OffsetMath（12个测试）
- ✅ RAW16K/64K/256K 布局计算
- ✅ Chunk/Block 偏移计算
- ✅ Meta/Data Region 偏移
- ✅ 边界条件与异常处理
- ✅ Extent 对齐辅助函数

### T3-StructSizeTest（13个测试）
- ✅ ContainerHeader = 16KB
- ✅ RawChunkHeader = 128 bytes
- ✅ BlockDirEntry = 48 bytes
- ✅ Field offsets 验证
- ✅ 初始化验证（flags=0xFFFFFFFF）
- ✅ State bit helpers

### T4-StateMachineTest（12个测试）
- ✅ Chunk header 初始化
- ✅ Chunk 生命周期（FREE→ALLOCATED→SEALED→DEPRECATED）
- ✅ Block 封存操作
- ✅ 状态位断言（monotonic time, no gaps）
- ✅ 防止重复操作
- ✅ 多 chunk 操作

## 布局计算示例

### RAW16K (256MB Chunk)
- **Block Size**: 16KB (1 extent)
- **Blocks per Chunk**: 16,384
- **Meta Blocks**: 48
- **Data Blocks**: 16,336
- **Directory Size**: ~784KB

### RAW64K (256MB Chunk)
- **Block Size**: 64KB (4 extents)
- **Blocks per Chunk**: 4,096
- **Meta Blocks**: 3
- **Data Blocks**: 4,093
- **Directory Size**: ~196KB

### RAW256K (256MB Chunk)
- **Block Size**: 256KB (16 extents)
- **Blocks per Chunk**: 1,024
- **Meta Blocks**: 1
- **Data Blocks**: 1,023
- **Directory Size**: ~49KB

## 编码规范

遵循 Google C++ Style Guide：
- **类名**：PascalCase（`AlignedIO`, `LayoutCalculator`）
- **函数名**：camelCase（`calculateLayout`, `isExtentAligned`）
- **变量名**：snake_case（`chunk_id`, `block_size`）
- **成员变量**：snake_case with trailing underscore（`fd_`, `stats_`）

## 开发路线图

### ✅ 阶段1：物理层与布局管理器（已完成）
- AlignedIO 类（16KB 对齐强制）
- LayoutCalculator（偏移量计算）
- T1/T2 测试通过（22个测试用例）

### ✅ 阶段2：头部定义与状态机（已完成）
- ContainerHeader / RawChunkHeader / BlockDirEntry
- StateMutator（SealBlock/SealChunk/Deprecate）
- Active-low 状态位（SSD 友好，1→0 only）
- T3/T4 测试通过（25个测试用例）

### 📋 阶段3：写入路径
- WALWriter（写前日志）
- MemBuffer（按 Tag 聚合）
- BlockWriter（高吞吐写入）

### 📋 阶段4：Seal 与目录构建
- DirectoryBuilder（集中目录管理）
- ChunkSealer（Chunk 封存）

### 📋 阶段5：读取与恢复
- RawScanner（脱库扫描工具）
- BlockReader（数据读取）
- 崩溃恢复测试

### 📋 阶段6：SQLite 集成
- MetadataSync（元数据同步）
- 端到端查询测试

## 性能指标

- **编译时间**：< 5秒
- **测试时间**：0.13秒（4个测试套件，47个测试用例）
- **内存开销**：零额外开销（纯计算）

## 技术文档

- **设计文档**：[docs/design.md](docs/design.md) - V1.6 完整设计规范
- **实施计划**：[docs/plan.md](docs/plan.md) - 6阶段开发计划
- **阶段1总结**：[docs/phase1_summary.md](docs/phase1_summary.md) - 物理层与布局管理器
- **阶段2总结**：[docs/phase2_summary.md](docs/phase2_summary.md) - 头部定义与状态机

## 许可证

内部项目

## 维护者

xTdb Development Team

---

**最后更新**：2026-01-02
**版本**：V1.6 (Phase 1 & 2 Completed)

