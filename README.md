# xTdb - Industrial TSDB Storage Core (V1.6)

高性能工业时序数据库存储内核，采用 RAW Fixed-Block Chunks + Central Directory 架构。

## 项目状态

✅ **Phase 1-10 完成**：核心存储引擎
- 物理层与布局管理（AlignedIO, LayoutCalculator）
- 头部定义与状态机（ContainerHeader, StateMutator）
- 写入路径（WAL, MemBuffer, BlockWriter）
- Seal 与目录构建（DirectoryBuilder, ChunkSealer）
- 读取与恢复（RawScanner, BlockReader）
- SQLite 集成（MetadataSync）
- 全局初始化与启动（Bootstrap Sequence）
- 写路径编排（WriteCoordinator）
- 读路径编排（ReadCoordinator）
- 后台服务（RetentionService, ChunkReclamation）

✅ **Phase 11 完成**：Public C API (2026-01-07)
- 25+ C API 函数，涵盖所有核心操作
- 线程安全，opaque handle 设计
- 完整示例和文档（examples/api_example.c）

🎯 **下一步**：Phase 12+ - PHD 压缩特性集成
- Swinging Door 压缩算法
- 16-bit 量化
- 多分辨率 Archive

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

## API 使用

### C API (推荐 - Phase 11)

xTdb 提供完整的 C API，支持跨语言集成（Python, Go, Rust 等）：

```c
#include <xTdb/xtdb_api.h>

// 1. 打开数据库
xtdb_config_t config;
xtdb_config_init(&config);
config.data_dir = "./my_data";
config.retention_days = 30;

xtdb_handle_t db = NULL;
xtdb_error_t err = xtdb_open(&config, &db);
if (err != XTDB_SUCCESS) {
    fprintf(stderr, "Error: %s\n", xtdb_error_string(err));
    return 1;
}

// 2. 写入数据
xtdb_point_t point = {
    .tag_id = 1001,
    .timestamp_us = get_current_time_us(),
    .value = 25.5,
    .quality = 192
};
xtdb_write_point(db, &point);
xtdb_flush(db);

// 3. 查询数据
xtdb_result_set_t result;
xtdb_query_points(db, 1001, start_time, end_time, &result);

size_t count = xtdb_result_count(result);
for (size_t i = 0; i < count; i++) {
    xtdb_point_t pt;
    xtdb_result_get(result, i, &pt);
    printf("Time: %lld, Value: %.2f\n", pt.timestamp_us, pt.value);
}
xtdb_result_free(result);

// 4. 关闭数据库
xtdb_close(db);
```

**更多示例**：见 `examples/api_example.c` 和 `examples/README.md`

### C++ API (底层接口)

#### 1. AlignedIO - 对齐 I/O 操作

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
│   ├── struct_defs.h      # 结构体定义（V1.6 - 支持 PHD 压缩）
│   ├── state_mutator.h    # 状态机
│   ├── storage_engine.h   # Phase 7-10：全局引擎
│   ├── xtdb_api.h         # ✨ Phase 11：C API 接口
│   └── ...（其他组件）
├── src/                   # 源文件
│   ├── aligned_io.cpp
│   ├── layout_calculator.cpp
│   ├── state_mutator.cpp
│   ├── storage_engine.cpp # Phase 7-10：引擎实现
│   ├── xtdb_api.cpp       # ✨ Phase 11：C API 实现
│   └── ...（其他组件）
├── examples/              # ✨ Phase 11：示例程序
│   ├── api_example.c      # C API 完整示例
│   └── README.md          # 示例文档
├── tests/                 # 测试文件（12+ 测试套件）
│   ├── test_alignment.cpp
│   ├── test_layout.cpp
│   ├── test_struct_size.cpp
│   ├── test_maintenance.cpp  # Phase 10
│   └── ...（其他测试）
├── docs/                  # 文档
│   ├── design.md          # V1.6 设计文档
│   ├── plan.md            # 实施计划
│   ├── phase1_summary.md  # 至 phase11_summary.md
│   ├── phd_integration_analysis.md       # PHD 特性分析
│   ├── phd_integration_preparation.md    # PHD 准备工作
│   ├── phd_integration_ready.md          # PHD 集成就绪确认
│   └── PHD_compression_and_storage_summary.md
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

### ✅ Phase 1-2：基础设施（已完成）
- 物理层与布局管理（AlignedIO, LayoutCalculator）
- 头部定义与状态机（Active-low 状态位）

### ✅ Phase 3-6：核心功能（已完成）
- 写入路径（WAL, MemBuffer, BlockWriter）
- Seal 与目录构建（DirectoryBuilder, ChunkSealer）
- 读取与恢复（RawScanner, BlockReader）
- SQLite 集成（MetadataSync）

### ✅ Phase 7-10：全局引擎与编排（已完成）
- 全局初始化与启动（Bootstrap Sequence）
- 写路径编排（WriteCoordinator）
- 读路径编排（ReadCoordinator）
- 后台服务（RetentionService, ChunkReclamation）

### ✅ Phase 11：公共 API 接口（2026-01-07 完成）
- C API 设计与实现（25+ 函数）
- 线程安全封装（per-handle mutex）
- 示例程序与文档（examples/api_example.c）

### 🎯 Phase 12+：PHD 压缩特性（下一步）
- Swinging Door 压缩算法
- 16-bit 量化
- 多分辨率 Archive 管理
- 质量加权聚合
- 预处理管道

## 性能指标

- **编译时间**：< 5秒
- **测试时间**：0.13秒（4个测试套件，47个测试用例）
- **内存开销**：零额外开销（纯计算）

## 技术文档

### 核心设计
- **设计文档**：[docs/design.md](docs/design.md) - V1.6 完整设计规范
- **实施计划**：[docs/plan.md](docs/plan.md) - 开发计划

### Phase 总结（Phase 1-11）
- [Phase 1: 物理层与布局管理器](docs/phase1_summary.md)
- [Phase 2: 头部定义与状态机](docs/phase2_summary.md)
- [Phase 3-9: 核心功能实现](docs/)
- [Phase 10: 后台维护服务](docs/phase10_summary.md)
- [Phase 11: 公共 C API 接口](docs/phase11_summary.md) ⭐ **最新**

### PHD 压缩特性集成
- [PHD 特性总结](docs/PHD_compression_and_storage_summary.md) - PHD 原理与机制
- [PHD 集成分析](docs/phd_integration_analysis.md) - 特性价值评估
- [PHD 准备工作](docs/phd_integration_preparation.md) - V1.6 结构性改造
- [PHD 集成就绪确认](docs/phd_integration_ready.md) - 当前状态与路线图

### API 文档
- **C API 参考**：[include/xTdb/xtdb_api.h](include/xTdb/xtdb_api.h) - 完整 API 文档
- **使用示例**：[examples/README.md](examples/README.md) - 集成指南
- **示例代码**：[examples/api_example.c](examples/api_example.c) - 完整演示

## 许可证

内部项目

## 维护者

xTdb Development Team

---

**最后更新**：2026-01-07
**版本**：V1.6 (Phase 1-11 Completed, API Ready)

