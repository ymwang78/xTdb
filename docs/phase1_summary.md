# xTdb Phase 1 Implementation Summary

## 完成时间
2026-01-02

## 实现概述

阶段1（物理层与布局管理器）已成功完成，所有测试通过。

## 实现的模块

### 1. 常量定义 (`include/xTdb/constants.h`)
- **Extent 定义**：16KB 最小物理分配/对齐单位
- **RAW Block Classes**：RAW_16K / RAW_64K / RAW_256K
- **辅助函数**：
  - `extentToBytes()` - Extent 转字节
  - `bytesToExtent()` - 字节转 Extent
  - `isExtentAligned()` - 检查对齐

### 2. AlignedIO 类 (`include/xTdb/aligned_io.h`, `src/aligned_io.cpp`)
**核心功能**：
- ✅ 封装 Linux 的 `pwrite/pread` 操作
- ✅ **强制约束**：Buffer、Size、Offset 必须 16KB 对齐
- ✅ 实现 `fallocate` 预分配（防止碎片）
- ✅ 支持 O_DIRECT（可选）
- ✅ I/O 统计（写入/读取字节数、操作次数）

**关键设计**：
- 违反对齐约束立即返回 `ERROR_ALIGNMENT`（不会写入磁盘）
- AlignedBuffer RAII 包装器自动管理 16KB 对齐内存

### 3. LayoutCalculator 类 (`include/xTdb/layout_calculator.h`, `src/layout_calculator.cpp`)
**核心功能**：
- ✅ 计算 Chunk 布局（MetaBlocks、DataBlocks 数量）
- ✅ 计算物理偏移量（Chunk/Block → 字节偏移）
- ✅ 支持三种 Block 尺寸（16K/64K/256K）
- ✅ 迭代求解 meta_blocks（收敛算法）

**关键公式实现**：
```cpp
// Chunk offset
chunk_offset = container_base + chunk_id * chunk_size_bytes

// Block offset
block_offset = chunk_base + block_index * block_size_bytes

// Meta blocks calculation (iterative)
meta_bytes = sizeof(ChunkHeader) + data_blocks * sizeof(BlockDirEntry)
meta_blocks = ceil(meta_bytes / block_size_bytes)
```

## 测试结果

### T1-AlignmentCheck（10个测试用例）
✅ **全部通过**（0.12秒）

测试覆盖：
1. ✅ 有效对齐写入成功
2. ✅ 未对齐 Buffer 失败
3. ✅ 未对齐 Size 失败
4. ✅ 未对齐 Offset 失败
5. ✅ 多次对齐写入
6. ✅ 对齐读取
7. ✅ 未对齐读取失败
8. ✅ Preallocate 对齐
9. ✅ Preallocate 未对齐失败
10. ✅ AlignedBuffer 自动对齐

### T2-OffsetMath（12个测试用例）
✅ **全部通过**（0.00秒）

测试覆盖：
1. ✅ RAW16K 256MB 布局（meta_blocks=48, data_blocks=16336）
2. ✅ RAW64K 256MB 布局（meta_blocks=3, data_blocks=4093）
3. ✅ RAW256K 256MB 布局（meta_blocks=1, data_blocks=1023）
4. ✅ Chunk 偏移计算
5. ✅ Meta Block 偏移计算
6. ✅ Data Block 偏移计算
7. ✅ BlockDir 偏移
8. ✅ Data Region 偏移
9. ✅ 无效 Block Class 异常
10. ✅ Chunk 小于 Block 异常
11. ✅ Block Index 越界异常
12. ✅ Extent 对齐辅助函数

## 编译与运行

```bash
# 构建并运行测试
./build.sh --test

# 仅构建
./build.sh
```

## 验证清单

| 要求 | 状态 | 说明 |
|------|------|------|
| 16KB 对齐强制 | ✅ | AlignedIO 在写入前验证对齐 |
| Offset 计算零误差 | ✅ | LayoutCalculator 通过所有边界测试 |
| fallocate 预分配 | ✅ | 实现在 AlignedIO::preallocate() |
| RAW16K 布局正确 | ✅ | meta_blocks=48, data_blocks=16336 |
| RAW64K 布局正确 | ✅ | meta_blocks=3, data_blocks=4093 |
| RAW256K 布局正确 | ✅ | meta_blocks=1, data_blocks=1023 |
| 单元测试覆盖 | ✅ | 22个测试用例全部通过 |

## 项目结构

```
xTdb/
├── include/xTdb/
│   ├── constants.h           # 常量定义
│   ├── aligned_io.h          # AlignedIO 类
│   └── layout_calculator.h   # LayoutCalculator 类
├── src/
│   ├── aligned_io.cpp
│   └── layout_calculator.cpp
├── tests/
│   ├── test_alignment.cpp    # T1-AlignmentCheck
│   └── test_layout.cpp       # T2-OffsetMath
├── docs/
│   ├── design.md             # V1.6 设计文档
│   ├── plan.md               # 实施计划
│   └── phase1_summary.md     # 本文档
├── CMakeLists.txt            # CMake 构建配置
└── build.sh                  # 构建脚本

build/                        # 构建目录（自动生成）
├── libxtdb_core.a            # 静态库
├── test_alignment            # 测试可执行文件
└── test_layout               # 测试可执行文件
```

## 下一步：阶段2

根据 `plan.md`，下一步是实现**头部定义与状态机**：

### 阶段2任务清单
1. **StructDefs.h**：
   - ContainerHeaderV12
   - RawChunkHeaderV16
   - BlockDirEntryV16

2. **StateMutator**：
   - SealBlock()
   - SealChunk()
   - DeprecateChunk()
   - Active-low 状态位操作（只允许 1→0）

3. **测试**：
   - T3-StructSize：验证结构体大小
   - T4-BitFlip：验证状态位只能 1→0

## 技术亮点

1. **零拷贝对齐约束**：在 I/O 之前强制验证，避免运行时错误
2. **迭代收敛算法**：自动计算最优 meta_blocks 数量
3. **类型安全**：使用 enum class 防止参数混淆
4. **RAII 内存管理**：AlignedBuffer 自动管理对齐内存
5. **完整测试覆盖**：22个测试用例，覆盖正常路径和异常情况

## 性能指标

- **编译时间**：< 5秒（使用 make -j）
- **测试运行时间**：0.13秒（全部测试）
- **内存开销**：零额外开销（纯计算函数）

## 附注

- 所有代码遵循 Google C++ Style Guide
- 类名：PascalCase（AlignedIO, LayoutCalculator）
- 函数名：camelCase（calculateLayout, isExtentAligned）
- 变量名：snake_case（chunk_id, block_size_bytes）
- 成员变量：snake_case with trailing underscore（fd_, stats_）
