# Industrial TSDB Storage Core Design (V1.1 / RAW SuperBlock Layout)

> 本文为存储内核 **V1.1 定稿版**（面向实现）。相对 V1：将 RAW Chunk 的头部升级为 **256KB Chunk SuperBlock**，并在其中集中存放 **Block Directory**；RAW 数据 Block 本体仅存 records（无 BlockHeader）。  
> 目标仍然是：写路径稳定、崩溃可恢复、可脱库扫描、可维护；压缩与复杂编码后置。

---

## 1. 设计目标

面向工业时序数据场景，构建单机 TSDB 存储内核：

- 高写入吞吐（百万点/分钟级）
- 顺序 IO 友好、低写放大
- 崩溃可恢复（WAL + 物理头部可验证）
- 易调试、可长期维护（支持脱离 SQLite 的扫描/修复）
- 后续可引入压缩与冷热分层（RAW → COMPACT）

核心原则：

> **Container 是物理承载实体；Extent 是物理对齐单位；Chunk/Block/Record 是逻辑组织单元。  
> V1.1：RAW 的“块对齐”优先，元数据集中化优先。**

---

## 2. 整体分层结构

```
┌─────────────────────────────────────────┐
│               Query / API               │
├─────────────────────────────────────────┤
│             SQLite 元数据索引            │
├─────────────────────────────────────────┤
│   Container / Chunk / Block 数据区       │
├─────────────────────────────────────────┤
│            WAL（写前日志，顺序）          │
└─────────────────────────────────────────┘
```

- **SQLite**：只存元数据索引；不参与高频写入。
- **WAL**：高频顺序写；用于崩溃恢复与延迟落盘。
- **物理头部（Container / Chunk SuperBlock）**：事实源，可校验、可脱库扫描。

---

## 3. 基本对齐单位

### 3.1 Extent（16KB）

- **Extent**：最小磁盘分配/对齐单位，建议 **16KB**。
- 所有 on-disk offset/length 均为 Extent 整数倍。

```c
#define EXTENT_SIZE_BYTES 16384u  // 16KB
```

---

## 4. 物理布局（RAW_FIXED / COMPACT_VAR）

### 4.1 Container（物理承载）

- Container：大文件或块设备。
- 一个 Container 内只允许一种 layout：RAW 或 COMPACT（不可混用）。

Container Header 固定 1 Extent（16KB），用于识别与 instance 校验。

```c
#define CONTAINER_MAGIC "XTSDBCON" // 8 bytes

enum ContainerLayout : uint8_t {
    LAYOUT_RAW_FIXED   = 1,
    LAYOUT_COMPACT_VAR = 2
};

enum CapacityType : uint8_t {
    CAP_DYNAMIC = 1,
    CAP_FIXED   = 2
};

struct ContainerHeaderV1 {
    char     magic[8];
    uint16_t version;            // = 1
    uint16_t header_size;        // = EXTENT_SIZE_BYTES

    uint8_t  db_instance_id[16]; // must match db_meta['instance_id']

    uint8_t  layout;             // ContainerLayout
    uint8_t  capacity_type;      // CapacityType
    uint16_t flags;

    uint64_t capacity_extents;   // CAP_FIXED 必填；CAP_DYNAMIC 可为 0
    int64_t  created_ts_us;      // PostgreSQL epoch us

    uint32_t header_crc32;       // CRC32(header without this field)
    uint32_t reserved0;

    // RAW: payload 可用于 chunk-slot bitmap（可选；也可挪到 SuperBlock 体系）
    uint8_t  payload[EXTENT_SIZE_BYTES - 8 - 2 - 2 - 16 - 1 - 1 - 2 - 8 - 8 - 4 - 4];
};
```

---

## 5. RAW_FIXED：Chunk = 256MB，Block = 256KB，SuperBlock + DataBlocks

### 5.1 常量定义

```c
#define RAW_CHUNK_SIZE_BYTES      (256ull * 1024 * 1024)   // 256MB
#define RAW_BLOCK_SIZE_BYTES      (256u  * 1024)           // 256KB

#define RAW_CHUNK_EXTENTS         (RAW_CHUNK_SIZE_BYTES / EXTENT_SIZE_BYTES) // 16384 extents
#define RAW_BLOCK_EXTENTS         (RAW_BLOCK_SIZE_BYTES / EXTENT_SIZE_BYTES) // 16 extents

#define RAW_BLOCKS_PER_CHUNK      (RAW_CHUNK_SIZE_BYTES / RAW_BLOCK_SIZE_BYTES) // 1024
#define RAW_SUPERBLOCK_INDEX      0u
#define RAW_FIRST_DATA_BLOCK      1u
#define RAW_DATA_BLOCKS_PER_CHUNK (RAW_BLOCKS_PER_CHUNK - 1u) // 1023
```

### 5.2 RAW Chunk 物理布局

对每个 chunk（槽位）：

- `block_index = 0`：**Chunk SuperBlock（256KB）**
  - 存放 ChunkHeader + Block Directory + 预留区
- `block_index = 1..1023`：**Data Block（每块 256KB）**
  - 仅存 record 序列（无 BlockHeader）

物理偏移计算（slot 映射保持简单）：

```text
chunk_base = container_base + chunk_id * RAW_CHUNK_SIZE_BYTES
block_offset_bytes(chunk_id, block_index) = chunk_base + block_index * RAW_BLOCK_SIZE_BYTES
```

> 结论：Chunk 与 Block 始终保持整数倍关系；ChunkHeader 不会破坏对齐，因为它被收纳在 SuperBlock 内。

---

## 6. Chunk SuperBlock（256KB）

### 6.1 Chunk 状态机（无 chunk_gen 版本）

ChunkHeader.flags 需包含以下状态（示意）：

- ACTIVE：正在写入（或当前热集）
- SEALED：封存只读
- DEPRECATED：逻辑下线（查询不可达），等待回收
- FREE：槽位空闲，可复用

> 重要：DEPRECATED/FREE 必须落盘在 **SuperBlock 的 ChunkHeader**，否则脱库扫描重建会“复活”旧数据。

### 6.2 SuperBlock 结构（建议）

SuperBlock 总大小固定 256KB。推荐在内部按区域划分：

```
+-----------------------------+ 0
| ChunkHeader (fixed, e.g 256)| 
+-----------------------------+
| Block Directory (1023 entries) |
+-----------------------------+
| Reserved / Future (padding) |
+-----------------------------+ 256KB
```

#### 6.2.1 ChunkHeader（放在 SuperBlock 开头）

```c
#define RAW_CHUNK_MAGIC "XTSRAWCK" // 8 bytes

enum ChunkFlags : uint32_t {
    CHF_ACTIVE     = 1u << 0,
    CHF_SEALED     = 1u << 1,
    CHF_DEPRECATED = 1u << 2,
    CHF_FREE       = 1u << 3
};

struct RawChunkHeaderV11 {
    char     magic[8];           // "XTSRAWCK"
    uint16_t version;            // = 0x0111 (example) or keep = 1 with layout tag
    uint16_t header_size;        // sizeof(RawChunkHeaderV11)

    uint8_t  db_instance_id[16];

    uint32_t chunk_id;           // slot id (reusable only after FREE)
    uint32_t flags;              // ChunkFlags

    uint32_t block_dir_entry_size;   // sizeof(BlockDirEntryV11)
    uint32_t data_blocks;            // = RAW_DATA_BLOCKS_PER_CHUNK (1023)

    int64_t  start_ts_us;        // seal 后写定（chunk 内最小时间）
    int64_t  end_ts_us;          // seal 后写定（chunk 内最大时间）

    uint32_t super_crc32;        // CRC32(superblock without this field)（推荐覆盖整个256KB）
    uint32_t reserved0;
};
```

- `super_crc32` 推荐覆盖整个 256KB（除自身字段），以便检测“半写/损坏”。

#### 6.2.2 Block Directory（每个 data block 一条）

Block Directory 索引范围：`data_index = 0..1022` 对应 `block_index = data_index + 1`。

Directory 的职责：

- 提供查询、扫描与恢复所需的最小元信息（tag_id、时间范围、record_count 等）
- **仅在 block seal 时写定**（避免高频随机写）

```c
enum ValueType : uint8_t {
    VT_BOOL  = 1,
    VT_I32   = 2,
    VT_F32   = 3,
    VT_F64   = 4
};

enum TimeUnit : uint8_t {
    TU_100MS = 1,
    TU_10MS  = 2,
    TU_MS    = 3,
    TU_100US = 4,
    TU_10US  = 5,
    TU_US    = 6
};

enum BlockFlags : uint32_t {
    BF_SEALED         = 1u << 0,
    BF_MONOTONIC_TIME = 1u << 1,
    BF_NO_TIME_GAP    = 1u << 2
};

struct BlockDirEntryV11 {
    uint32_t tag_id;

    uint8_t  value_type;         // ValueType
    uint8_t  time_unit;          // TimeUnit
    uint16_t record_size;        // bytes per record (time_offset + quality + value)

    uint32_t flags;              // BlockFlags

    int64_t  start_ts_us;        // block 基准时间（pg epoch us）
    int64_t  end_ts_us;          // seal 后写定

    uint32_t record_count;       // seal 后写定；未 seal = 0xFFFFFFFF
    uint32_t data_crc32;         // optional
};
```

- `bytes_used = record_size * record_count`（seal 后可计算）
- 未 seal：`record_count=0xFFFFFFFF`

---

## 7. Data Block（256KB，仅 records，无 header）

### 7.1 Record 格式

```
┌──────────────┬──────────┬───────────────┐
│ time_offset  │ quality  │ value         │
│   3 bytes    │ 1 byte   │ N bytes        │
└──────────────┴──────────┴───────────────┘
```

- `time_offset`：3B，小端；相对 `BlockDirEntry.start_ts_us`；单位为 `time_unit`
- `quality`：1B，高 2 bit 主质量（Invalid/Good/Bad/Uncertain），低 6 bit 自定义
- `value`：N bytes，由 `value_type` 决定（建议 BOOL 仍占 1 byte）

---

## 8. 写入路径与 Seal（V1.1）

### 8.1 高频写入路径

- 数据先顺序写入 **WAL**
- 内存按 tag 聚合缓冲
- 达到阈值（写满 256KB、时间窗、或策略）后，将 records 顺序写入对应 Data Block

### 8.2 Seal（低频一次写定）

- **Block seal**：写定该 block 的 BlockDirEntry：
  - `end_ts_us`
  - `record_count`
  - `flags`
  - `data_crc32`（可选）
- **Chunk seal**：写定 ChunkHeader：
  - `CHF_SEALED`
  - `start_ts_us / end_ts_us`
  - `super_crc32`

> 约束：Directory/ChunkHeader 不在每条 record 追加时更新；只在 seal 时批量写定。

### 8.3 崩溃恢复策略

- 明确 WAL 恢复窗口（例如最近 24h 或最近 K 段）
- 未 seal 的 block 可丢弃并通过 WAL 重放恢复（V1.1 推荐默认策略）

---

## 9. RAW Chunk 回收复用（无 chunk_gen）

### 9.1 前提：WAL 恢复窗口

- WAL 只恢复最近 N 小时/段
- 回收复用仅针对远超窗口的历史 chunk

### 9.2 顺序（确定性）

**Phase A：逻辑下线**
1. SuperBlock：写 `CHF_DEPRECATED` 并落盘（更新 `super_crc32`）
2. SQLite `chunks.flags` 同步置 DEPRECATED（建议）

**Phase B：清引用**
1. 读者退出屏障（refcount/epoch）
2. SQLite 事务：删除 blocks 引用并提交

**Phase C：延迟回收**
1. 可设置安全期（24~48h）
2. 置 FREE：
   - SuperBlock：写 `CHF_FREE` 并落盘（更新 crc）
   -（可选）Container bitmap：chunk_id 置可用

**Phase D：复用**
1. 写新 SuperBlock（CHF_ACTIVE + 清空目录）
2. 写 data blocks

---

## 10. SQLite 元数据索引（V1.1）

SQLite 为加速查询，镜像存放 chunks/blocks 信息；事实源仍可由 SuperBlock 扫描重建。

```sql
CREATE TABLE IF NOT EXISTS containers (
    container_id       INTEGER PRIMARY KEY,
    container_type     INTEGER NOT NULL,  -- 1=FILE 2=BIGFILE 3=BLOCK_DEVICE
    container_path     TEXT    NOT NULL,
    layout             INTEGER NOT NULL,  -- 1=RAW_FIXED 2=COMPACT_VAR
    capacity_type      INTEGER NOT NULL,  -- 1=DYNAMIC 2=FIXED
    capacity_extents   INTEGER,
    flags              INTEGER NOT NULL,
    created_ts_us      INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS chunks (
    container_id       INTEGER NOT NULL,
    chunk_type         INTEGER NOT NULL,  -- 1=RAW 2=COMPACT
    chunk_id           INTEGER NOT NULL,  -- RAW slot id
    chunk_off_extents  INTEGER NOT NULL,  -- RAW: chunk_id * RAW_CHUNK_EXTENTS
    chunk_extents      INTEGER NOT NULL,  -- RAW: RAW_CHUNK_EXTENTS
    start_ts_us        INTEGER,
    end_ts_us          INTEGER,
    block_count        INTEGER NOT NULL,
    flags              INTEGER NOT NULL,
    created_ts_us      INTEGER NOT NULL,
    sealed_ts_us       INTEGER,

    PRIMARY KEY (container_id, chunk_type, chunk_id)
);

CREATE TABLE IF NOT EXISTS blocks (
    block_id           INTEGER PRIMARY KEY,
    tag_id             INTEGER NOT NULL,

    container_id       INTEGER NOT NULL,
    chunk_type         INTEGER NOT NULL,
    chunk_id           INTEGER NOT NULL,

    block_index        INTEGER NOT NULL,  -- RAW: 1..1023
    block_off_extents  INTEGER NOT NULL,  -- chunk_off + block_index * RAW_BLOCK_EXTENTS
    block_extents      INTEGER NOT NULL,  -- RAW fixed = RAW_BLOCK_EXTENTS

    start_ts_us        INTEGER NOT NULL,
    end_ts_us          INTEGER NOT NULL,

    record_count       INTEGER NOT NULL,
    value_type         INTEGER NOT NULL,
    time_unit          INTEGER NOT NULL,
    record_size        INTEGER NOT NULL,
    flags              INTEGER NOT NULL,

    FOREIGN KEY(container_id, chunk_type, chunk_id) REFERENCES chunks(container_id, chunk_type, chunk_id)
);

CREATE INDEX IF NOT EXISTS idx_blocks_tag_start
ON blocks(tag_id, start_ts_us);

CREATE INDEX IF NOT EXISTS idx_blocks_chunk
ON blocks(container_id, chunk_type, chunk_id);
```

---

## 11. 演进约定

- SuperBlock 预留区用于未来：双写（seq+crc）、摘要索引、目录扩展等。
- COMPACT_VAR 可采用不同布局（可变长 chunk、压缩 block、packing），与 RAW_FIXED 分离演进。

