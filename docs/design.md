# Industrial TSDB Storage Core Design (V1.6 / RAW Fixed-Block Chunks + Central Directory)

> 本文为存储内核 **V1.6 定稿版**（面向实现）。
> 1) 统一最小物理分配/对齐单位 **Extent=16KB**，因此 **最小 Block=16KB**（放弃 4KB）。  
> 2) RAW 不再在 Chunk 内混杂多种 Block 尺寸；改为 **按 Block 尺寸分池（ChunkType）**：RAW16K / RAW64K / RAW256K。  
> 3) 采用 **集中目录（Central Directory）**：每个 Chunk 前部预留“元数据区”，存放 ChunkHeader + BlockDir（目录项）；数据 Block 本体仅存 records（无 BlockHeader）。

---

## 1. 设计目标

面向工业时序数据场景，构建单机 TSDB 存储内核：

- 高写入吞吐（百万点/分钟级）
- 顺序 IO 友好、低写放大
- 崩溃可恢复（WAL + 物理头部可验证）
- 易调试、可长期维护（支持脱离 SQLite 的扫描/修复）
- 后续可引入压缩与冷热分层（RAW → COMPACT）

核心原则：

> **Container 是物理承载实体；Extent 是最小物理对齐单位；Chunk/Block/Record 是逻辑组织单元。  
> 写路径稳定优先；复杂压缩/编码后置。**

---

## 2. 基本单位

### 2.1 Extent（16KB）

- **Extent**：最小磁盘分配/对齐单位，固定 **16KB**。
- 所有 on-disk offset/length 必须是 Extent 的整数倍。

```c
#define EXTENT_SIZE_BYTES 16384u  // 16KB
```

### 2.2 RAW Block 尺寸集合（固定）

- **Block** 是最小写入与索引单位（对 tag/time 查询）。
- RAW 仅支持以下固定尺寸（均为 Extent 整数倍）：

| BlockClass | bytes | extents |
|---|---:|---:|
| RAW16K  | 16KB  | 1 |
| RAW64K  | 64KB  | 4 |
| RAW256K | 256KB | 16 |

---

## 3. 总体分层结构

```
┌─────────────────────────────────────────┐
│               Query / API               │
├─────────────────────────────────────────┤
│             SQLite 元数据索引            │
├─────────────────────────────────────────┤
│       RAW Containers (per BlockClass)   │
│   Container / Chunk / Block 数据区       │
├─────────────────────────────────────────┤
│            WAL（写前日志，顺序）          │
└─────────────────────────────────────────┘
```

---

## 4. Container（推荐：每种 BlockClass 一个 Container）

为了避免“同一文件内多种 chunk_size/block_size”的复杂度，V1.2 推荐：

- **一个 DB 实例内可以有多个 RAW Container**；
- 每个 RAW Container 固定一种 **BlockClass** 与对应的 **Chunk 固定大小**（slot size）。

> 这样：chunk_id → 物理偏移计算始终为“乘一个常量”，回收复用与扫描恢复最简单。

### 4.1 ContainerHeader（16KB，固定）

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

enum RawBlockClass : uint8_t {
    RBC_16K  = 1,
    RBC_64K  = 2,
    RBC_256K = 3
};

struct ContainerHeaderV12 {
    char     magic[8];
    uint16_t version;            // = 0x0102
    uint16_t header_size;        // = EXTENT_SIZE_BYTES

    uint8_t  db_instance_id[16]; // must match db_meta['instance_id']

    uint8_t  layout;             // RAW_FIXED / COMPACT_VAR
    uint8_t  capacity_type;      // DYNAMIC / FIXED
    uint16_t flags;

    uint64_t capacity_extents;   // CAP_FIXED 必填；CAP_DYNAMIC 可为 0
    int64_t  created_ts_us;      // PostgreSQL epoch us

    // RAW 固定参数（仅当 layout=RAW_FIXED 有意义）
    uint8_t  raw_block_class;    // RawBlockClass
    uint8_t  reserved8[7];

    uint32_t chunk_size_extents; // RAW: 固定 chunk slot 大小（以 extent 计）
    uint32_t block_size_extents; // RAW: 固定 block 大小（以 extent 计）

    uint32_t header_crc32;       // CRC32(header without this field)
    uint32_t reserved0;

    uint8_t  payload[EXTENT_SIZE_BYTES - 8 - 2 - 2 - 16 - 1 - 1 - 2 - 8 - 8 - 1 - 7 - 4 - 4 - 4 - 4];
};
```

---

## 5. RAW Chunk（固定 chunk_size，集中目录）

### 5.1 关键思想

- **Chunk 内 BlockSize 固定**（由 ContainerHeader 指定）。
- Chunk 的开头预留一个 **元数据区（Meta Region）**，由若干个“元数据块”组成（大小同 block_size）。
- 元数据区包含：
  - ChunkHeader（状态：ACTIVE/SEALED/DEPRECATED/FREE）
  - Block Directory（每个 data block 一条目录项）
- **数据区（Data Region）**由若干个 data blocks 组成，每个 data block 仅存 records（无 block header）。

### 5.2 Chunk 内布局

设：

- `BLOCK_BYTES = block_size_extents * EXTENT_SIZE_BYTES`
- `CHUNK_BYTES = chunk_size_extents * EXTENT_SIZE_BYTES`
- `BLOCKS_PER_CHUNK = CHUNK_BYTES / BLOCK_BYTES`

布局：

- `block_index = 0 .. META_BLOCKS-1`：Meta Region（若干块）
- `block_index = META_BLOCKS .. BLOCKS_PER_CHUNK-1`：Data Region（数据块）

物理偏移（slot 映射保持简单）：

```text
chunk_base = container_base + chunk_id * CHUNK_BYTES
block_offset_bytes(chunk_id, block_index) = chunk_base + block_index * BLOCK_BYTES
```

### 5.3 为什么需要多个 Meta Blocks

当 block_size 较小（例如 16KB）时，chunk 内 blocks 数量大；集中目录需要更多空间。  
因此 V1.2 将“Meta Region”定义为可变块数，但仍保持“按 block_size 对齐”。

---

## 6. ChunkHeader（集中目录体系的事实源，SSD 友好状态位）

ChunkHeader 放在 Meta Region 的第 0 块开头（block_index=0）。

```c
#define RAW_CHUNK_MAGIC "XTSRAWCK" // 8 bytes

// flags 采用 active-low / write-once：初始化全 1；状态推进只清 bit（1->0）。

enum ChunkStateBit : uint8_t {
    // 你要求：FREE -> ... -> DEPRECATED 的推进应体现 bit1->bit0 的清零过程
    // 因此将生命周期关键位布置为：allocated=bit2, sealed=bit1, deprecated=bit0
    CHB_DEPRECATED = 0,  // 1=未下线 0=已下线
    CHB_SEALED     = 1,  // 1=未封存 0=已封存
    CHB_ALLOCATED  = 2,  // 1=未分配(FREE候选) 0=已分配
    CHB_FREE_MARK  = 3   // 1=未标记FREE 0=已标记FREE（仅工具用途，可选）
};

static const uint32_t CH_FLAGS_INIT = 0xFFFFFFFFu;

static inline uint32_t ch_clear(uint32_t flags, ChunkStateBit b) {
    return flags & ~(1u << (uint32_t)b); // only 1->0
}

struct RawChunkHeaderV16 {
    char     magic[8];           // "XTSRAWCK"
    uint16_t version;            // = 0x0106
    uint16_t header_size;        // sizeof(RawChunkHeaderV16)

    uint8_t  db_instance_id[16];

    uint32_t chunk_id;           // slot id（CH_FLAGS_INIT 时可视为 FREE 候选）
    uint32_t flags;              // active-low 状态位，初始化 CH_FLAGS_INIT

    uint32_t chunk_size_extents; // 冗余写入（便于脱库扫描校验）
    uint32_t block_size_extents; // 冗余写入

    uint32_t meta_blocks;        // Meta Region 占用的 block 数（>=1）
    uint32_t data_blocks;        // Data block 数（= BLOCKS_PER_CHUNK - meta_blocks）

    int64_t  start_ts_us;        // seal 后写定（chunk 内最小时间）
    int64_t  end_ts_us;          // seal 后写定（chunk 内最大时间）

    uint32_t super_crc32;        // CRC32(meta region)（实现固定口径）
    uint32_t reserved0;
};
```

> 重要：DEPRECATED/FREE 必须写入 ChunkHeader 并落盘，否则脱库扫描重建会“复活”旧 chunk。

---

## 7. Block Directory（集中目录项）

### 7.1 目录项索引映射

- `data_index = 0 .. data_blocks-1`
- `block_index = meta_blocks + data_index`

目录项按 `data_index` 顺序排列，存放在 Meta Region 中（紧随 ChunkHeader）。


### 7.2 目录项定义

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

enum BlockStateBit : uint8_t {
    BLB_SEALED         = 0, // 1=未封存 0=已封存
    BLB_MONOTONIC_TIME = 1, // 1=未断言 0=断言时间单调
    BLB_NO_TIME_GAP    = 2  // 1=未断言 0=断言无漏点
};

static const uint32_t BL_FLAGS_INIT = 0xFFFFFFFFu;

static inline uint32_t bl_clear(uint32_t flags, BlockStateBit b) {
    return flags & ~(1u << (uint32_t)b);
}

struct BlockDirEntryV16 {
    uint32_t tag_id;

    uint8_t  value_type;
    uint8_t  time_unit;
    uint16_t record_size;

    uint32_t flags;              // active-low，初始化 BL_FLAGS_INIT

    int64_t  start_ts_us;
    int64_t  end_ts_us;          // seal 后写定

    uint32_t record_count;       // seal 后写定；未 seal = 0xFFFFFFFF
    uint32_t data_crc32;         // 可选：seal 后写定
};
```

### 7.3 META_BLOCKS 的计算（固定公式）

令：

- `dir_bytes = data_blocks * sizeof(BlockDirEntryV12)`
- `meta_bytes = sizeof(RawChunkHeaderV12) + dir_bytes`

则：

```text
meta_blocks = ceil(meta_bytes / BLOCK_BYTES)
data_blocks = BLOCKS_PER_CHUNK - meta_blocks
```

实现上应先确定 `chunk_size_extents` 与 `block_size_extents`，再据此计算 `meta_blocks/data_blocks`，并写入 ChunkHeader。

---

## 8. Data Block（固定大小，仅 records，无 header）

### 8.1 Record 格式（与既有一致）

```
┌──────────────┬──────────┬───────────────┐
│ time_offset  │ quality  │ value         │
│   3 bytes    │ 1 byte   │ N bytes        │
└──────────────┴──────────┴───────────────┘
```

- `time_offset`：3B，小端；相对 `BlockDirEntry.start_ts_us`；单位为 `time_unit`
- `quality`：1B，高 2 bit 主质量（Invalid/Good/Bad/Uncertain），低 6 bit 自定义
- `value`：N bytes，由 `value_type` 决定

> 由于 data block 无 header，读取必须依赖 BlockDirEntry。

---

## 9. 写入路径与 Seal（集中目录的写放大控制）

### 9.1 高频写入路径（不触碰目录）

- 采集数据先顺序写入 **WAL**
- 内存按 tag 聚合缓冲
- 缓冲达到阈值后，将 records 顺序写入当前 data block（数据区）

### 9.2 Seal（低频更新目录）

- **Block seal**（低频）：写定对应 `BlockDirEntry` 的：
  - `end_ts_us`
  - `record_count`
  - `flags`
  - `data_crc32`（可选）
- **Chunk seal**（更低频）：写定 ChunkHeader：
  - `CHF_SEALED`
  - `start_ts_us / end_ts_us`
  - `super_crc32`

> 约束：目录项只在 seal 时写一次，避免“每条 record 更新元数据”的随机写放大。

### 9.3 BlockClass 选择（跨 chunk 池升级）

- tag 初始写入到 **RAW16K**（默认）
- 若写入速率较高导致 block 频繁 roll（例如 <2 分钟写满），升级到 RAW64K 或 RAW256K
- V1.2 建议只升级不降级（显著降低复杂度）

---

## 10. 崩溃恢复（事实源与重建）

- WAL 定义恢复窗口（例如最近 24h）
- 未 seal 的 block：可丢弃并通过 WAL 重放恢复（V1.2 推荐默认策略）
- SQLite 损坏时：可扫描各 Chunk 的 Meta Region（ChunkHeader + BlockDirEntry）重建索引

---

## 11. RAW Chunk 回收复用（无 chunk_gen，慢动作）

保持既定顺序：

1. ChunkHeader 写 `CHF_DEPRECATED` 并落盘
2. SQLite 删除该 chunk 的 blocks 记录并提交
3. 经过安全期（可选）后写 `CHF_FREE` 并落盘
4. 复用时重写 ChunkHeader（CHF_ACTIVE）并清空目录区（或用版本/seq 防旧目录误读）

---

## 12. SQLite 元数据索引（V1.2）

为加速查询镜像存放；事实源仍可由集中目录扫描重建。

建议关键字段新增：`raw_block_class` 或 `block_size_extents`（二者选其一；通常存 class 即可）。

```sql
CREATE TABLE IF NOT EXISTS containers (
    container_id       INTEGER PRIMARY KEY,
    container_path     TEXT    NOT NULL,
    layout             INTEGER NOT NULL,  -- 1=RAW_FIXED 2=COMPACT_VAR
    capacity_type      INTEGER NOT NULL,  -- 1=DYNAMIC 2=FIXED
    capacity_extents   INTEGER,
    raw_block_class    INTEGER,           -- RAW: 1=16K 2=64K 3=256K
    chunk_size_extents INTEGER,           -- RAW: fixed
    block_size_extents INTEGER,           -- RAW: fixed
    flags              INTEGER NOT NULL,
    created_ts_us      INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS chunks (
    container_id       INTEGER NOT NULL,
    chunk_id           INTEGER NOT NULL,  -- RAW slot id
    chunk_off_extents  INTEGER NOT NULL,  -- chunk_id * chunk_size_extents, DYNAMIC场景需要
    start_ts_us        INTEGER,
    end_ts_us          INTEGER,
    data_blocks        INTEGER NOT NULL,
    meta_blocks        INTEGER NOT NULL,
    flags              INTEGER NOT NULL,
    created_ts_us      INTEGER NOT NULL,
    sealed_ts_us       INTEGER,

    PRIMARY KEY (container_id, chunk_id),
);

-- 推荐：按月分表 blocks_202310
-- 注意：不需要 FOREIGN KEY，在大规模分表场景下 FK 会极大拖累性能且难以维护
CREATE TABLE IF NOT EXISTS blocks_202310 (
    -- 1. 核心查询键
    tag_id       INTEGER NOT NULL,
    start_ts_us  INTEGER NOT NULL,
    end_ts_us    INTEGER NOT NULL, -- 保留结束时间，用于时间范围查询的精确过滤

    -- 2. 物理定位键 (你的修改)
    container_id INTEGER NOT NULL,
    chunk_id     INTEGER NOT NULL,
    block_index  INTEGER NOT NULL, -- = offset / 16384 (直接存第几块)

    -- 3. 主键 (聚簇索引，加速查询)
    PRIMARY KEY (tag_id, start_ts_us)
) WITHOUT ROWID;

-- 4. 辅助索引 (加速运维/回收)
-- 当你要删除某个 Chunk 时，这个索引让 DELETE 操作瞬间完成
CREATE INDEX IF NOT EXISTS idx_blocks_202310_gc 
ON blocks_202310(container_id, chunk_id);
```

---

## 13. 默认参数建议（可直接实现）

在不引入复杂二级目录的前提下，推荐 chunk_size 选择使目录区开销可控：

- RAW256K：chunk_size = 256MB（blocks_per_chunk=1024，目录非常小）
- RAW64K ：chunk_size = 256MB（blocks_per_chunk=4096，目录中等）
- RAW16K ：chunk_size = 256MB（blocks_per_chunk=8192，meta_blocks 增加）

> 以上仅影响“一个 chunk 内目录项数量与 meta_blocks”，不会改变“chunk 内 block 固定”的原则。


---

## 14. SSD 友好的 Flags（只允许 1→0 写入）

Flash/SSD 的页编程天然支持 **bit 从 1 变 0**，而 **0→1 需要擦除**（通常是更大粒度的 erase block）。  
因此：**所有可变 Flags/状态字段应采用“active-low / write-once”编码**：

- **初始化为全 1**（`0xFF` / `0xFFFF` / `0xFFFFFFFF`），代表“默认/未发生”
- 状态推进时只做 **按位清零**（`new = old & mask`）
- 需要回到 FREE（全 1）时，不做 0→1 回写，而是：
  - 文件场景：依赖文件删除/打洞（punch hole）触发 TRIM（视文件系统/挂载选项而定）
  - 块设备场景：显式对 chunk 槽位范围做 **DISCARD/TRIM/UNMAP**（例如 Linux `BLKDISCARD`），使介质回到“擦除态”（全 1）

### 14.1 Chunk 状态位编码（推荐）

将 ChunkHeader.flags 的低位定义为 **write-once 状态位**（清零即“发生”）：

| 位 | 名称 | 1 表示 | 0 表示（发生） |
|---:|---|---|---|
| bit0 | CH_ALLOCATED | 未分配（FREE 候选） | 已分配（ACTIVE 或更后续状态） |
| bit1 | CH_SEALED | 未封存 | 已封存（只读） |
| bit2 | CH_DEPRECATED | 未下线 | 已下线（查询不可达，等待回收） |
| bit3 | CH_FREE_MARK | 未标记 FREE | 已标记 FREE（仅用于扫描工具；复用前仍建议 TRIM） |

约定：

- **FREE 初始态**：`flags = 0xFFFFFFFF`（全 1）
- **ACTIVE 分配**：清 `bit0` → `flags &= ~CH_ALLOCATED`
- **SEALED**：清 `bit1`
- **DEPRECATED**：清 `bit2`
- **FREE 标记**：清 `bit3`（可选；主要用于脱库扫描时快速识别“已释放槽位”）

> 说明：真正“可复用”的 FREE，推荐在进入复用队列前对该 chunk 槽位做 TRIM/DISCARD，使介质回到擦除态（全 1），从而允许写入新头部与新目录。  
> 若环境不支持 TRIM，则可采用“多 Header Slot”轮转写入以避免 0→1（V2 再引入）。

### 14.2 BlockDirEntry.flags（同样采用 write-once）

BlockDirEntryV12.flags 建议也按“清零表示发生”：

| 位 | 名称 | 1 表示 | 0 表示（发生） |
|---:|---|---|---|
| bit0 | BL_SEALED | 未封存 | 已封存 |
| bit1 | BL_MONOTONIC_TIME | 未断言单调 | 断言时间单调（可用于加速查询） |
| bit2 | BL_NO_TIME_GAP | 未断言无漏点 | 断言无漏点（可用于压缩/优化） |

初始化：`flags = 0xFFFFFFFF`，seal 时一次性按需清零。

### 14.3 “一次写定”字段初始化建议

为了避免高频回写导致 0→1 问题，以下字段应“只在 seal 时写一次”，并初始化为全 1：

- `BlockDirEntry.end_ts_us`：初始化 `0x7FFF...` 或全 1（按实现选定），seal 写定
- `BlockDirEntry.record_count`：初始化 `0xFFFFFFFF`，seal 写定
- `data_crc32`：初始化 `0xFFFFFFFF`，seal 写定
- `ChunkHeader.start_ts_us/end_ts_us`：初始化全 1，chunk seal 写定
- `super_crc32`：初始化全 1，seal 写定

