# PHD 压缩与分类存储特性吸纳分析与 xTdb 兼容性改造方案

> 文档版本：V1.0
> 创建日期：2026-01-07
> 目标：分析 PHD 值得吸纳的特性，并提出 xTdb 当前设计的改造方案

---

## 1. PHD 核心特性价值评估

### 1.1 高价值特性（强烈推荐吸纳）

#### ⭐⭐⭐ **Swinging Door 压缩算法（受限斜率）**
**价值分析**：
- 工业时序数据的核心特点：平稳变化 + 有工程容差
- 压缩比极高（对平稳 PV 可达 10:1 至 100:1）
- 保证误差有界（由工程容差控制）
- 查询时可精确重建（线性插值）

**技术关键点**：
- 按 Tag 配置容差（Tolerance × CompressionFactor）
- 存储的是线性段的关键端点，而非所有采样点
- 允许的斜率包络（slope envelope）判定

**吸纳难度**：★★★（中等）
- 需要引入新的编码格式
- 需要实现线性段存储与插值读取
- 与现有 RAW 格式兼容需要设计

---

#### ⭐⭐⭐ **16-bit 量化存储**
**价值分析**：
- 存储体积直接减少 50%~75%（相比 float32/float64）
- 精度损失仅 0.0015%（工业场景完全可接受）
- 适用于有明确工程量程的数据（绝大多数工业数据）

**技术关键点**：
- 为每个 Tag 配置 High/Low Extreme（物理量程）
- 写入时：`stored_value = (actual - low) / (high - low) * 65535`
- 读取时：反向线性映射还原

**吸纳难度**：★★（简单）
- 实现简单，只需线性映射
- 与现有 ValueType（VT_F32/VT_F64）并存
- 可新增 VT_QUANTIZED_16 类型

---

#### ⭐⭐ **多分辨率/分层 Archive**
**价值分析**：
- 解决长期存储成本问题（高频短期 + 低频长期）
- 查询透明化（API 自动选择最佳分辨率）
- 降低查询长周期数据的 I/O 开销

**技术架构**：
```
Layer 1 (Raw):       最近 3 个月，1秒级，原始数据
Layer 2 (Resampled): 最近 2 年，1分钟级，降采样
Layer 3 (Aggregated): 5 年以上，1小时级，统计聚合
```

**吸纳难度**：★★★★（较高）
- 需要重新设计 Container/Chunk 的层级管理
- 需要实现自动重采样/降采样机制
- 查询路由需要智能选择 Archive

---

#### ⭐⭐ **按数据类型分类的 Archive**
**价值分析**：
- SCAN（连续数值）、CHAR（字符串）、MANV（人工录入）分离
- 不同类型的数据具有不同的访问模式和生命周期
- 优化读写性能，避免混合存储的开销

**技术关键点**：
- 连续数值数据（高频、高压缩性）
- 字符串/二进制数据（低频、RLE 压缩）
- 人工录入数据（极低频、高重要性）

**吸纳难度**：★★★（中等）
- 当前 xTdb 已支持不同 ValueType
- 需要在 Container 层面区分数据类型
- 可通过不同 Container 实现逻辑分离

---

#### ⭐ **质量与置信度模型**
**价值分析**：
- 为每个数据点/线性段维护质量信息（0-100）
- 在缺测、插值、降采样时动态调整
- 聚合计算时按质量加权

**技术关键点**：
- 100：原始采集数据
- <100：插值/降采样数据
- 0：通信中断/传感器故障

**吸纳难度**：★★（简单）
- 当前 xTdb 的 quality 字段（1 byte）已有
- 可扩展为置信度语义
- 需要在聚合计算时使用质量加权

---

### 1.2 中等价值特性（选择性吸纳）

#### **写入前预处理（Pre-Processing）**
- 毛刺剔除（Gross Error Detection）
- 指数平滑（Exponential Smoothing）
- 死区/Gating（微小变化抑制）

**价值**：提高压缩率，降低噪声
**吸纳难度**：★★★（中等，属于 Ingestion Policy）

#### **重采样存储（Resampled Archive）**
- 固定周期重采样
- 独立 Archive 存储
- 降低长期存储成本

**价值**：与多分辨率 Archive 配合使用
**吸纳难度**：★★★（中等）

---

## 2. xTdb 当前设计的兼容性改造方案

### 2.1 立即修改（Phase 3 之前完成）

#### **改造 1：BlockDirEntry 增加编码类型字段**

**当前问题**：
- `BlockDirEntry` 没有编码类型字段
- 无法区分未来的压缩算法（Swinging Door、Quantized、Gorilla）

**改造方案**：
```cpp
enum EncodingType : uint8_t {
    ENC_RAW              = 0,  // 无压缩，直接存储
    ENC_SWINGING_DOOR    = 1,  // Swinging Door 算法
    ENC_QUANTIZED_16     = 2,  // 16-bit 量化
    ENC_GORILLA          = 3,  // Gorilla/XOR 压缩
    ENC_DELTA_OF_DELTA   = 4,  // Delta-of-Delta
    ENC_RESERVED_5       = 5,  // 预留
    ENC_RESERVED_6       = 6,  // 预留
    ENC_RESERVED_7       = 7   // 预留
};

struct BlockDirEntryV16 {
    uint32_t tag_id;

    uint8_t  value_type;        // VT_BOOL/I32/F32/F64
    uint8_t  encoding_type;     // ⭐ 新增：编码类型
    uint8_t  time_unit;
    uint8_t  reserved_u8;       // 对齐

    uint32_t flags;

    int64_t  start_ts_us;
    int64_t  end_ts_us;

    uint32_t record_count;
    uint32_t data_crc32;

    // ⭐ 新增：编码参数区（可选）
    uint32_t encoding_param1;   // 例如：Swinging Door 的 tolerance
    uint32_t encoding_param2;   // 例如：Quantized 的 low_extreme
};
```

**影响**：
- `BlockDirEntry` 大小从 48 bytes → 56 bytes
- Meta Region 会略微增大，但可接受
- **立即修改，避免后续重构**

---

#### **改造 2：SQLite 元数据增加 Tags 表（含压缩配置）**

**当前问题**：
- 没有 `tags` 表定义（`blocks` 表中使用了 `tag_id`，但没有主表）
- 没有地方存储 Tag 的元数据（名称、类型、单位、量程等）
- 无法支持 Swinging Door 的 Tolerance、16-bit 量化的 Extreme

**改造方案**：
```sql
-- 新增：Tags 主表（包含压缩配置）
CREATE TABLE IF NOT EXISTS tags (
    tag_id              INTEGER PRIMARY KEY AUTOINCREMENT,

    -- Tag 基本信息
    tag_name            TEXT NOT NULL UNIQUE,  -- 测点名称（如 "T101_Temperature"）
    tag_desc            TEXT,                  -- 描述

    -- 数据类型与单位
    value_type          INTEGER NOT NULL,      -- ValueType 枚举：VT_BOOL/I32/F32/F64
    unit                TEXT,                  -- 工程单位（如 "℃", "MPa", "m³/h"）

    -- 物理量程（用于量化压缩）
    low_extreme         REAL,                  -- 物理下限
    high_extreme        REAL,                  -- 物理上限

    -- 压缩配置
    preferred_encoding  INTEGER DEFAULT 0,     -- EncodingType：0=RAW, 1=SWINGING_DOOR, 2=QUANTIZED_16
    tolerance           REAL,                  -- Swinging Door 工程容差（物理单位）
    compression_factor  REAL DEFAULT 1.0,      -- 压缩因子（1.0 = 使用 tolerance）

    -- 预处理策略
    enable_gross_error_removal BOOLEAN DEFAULT 0,
    gross_error_stddev         REAL DEFAULT 3.0,  -- 标准差倍数（3σ）

    enable_smoothing           BOOLEAN DEFAULT 0,
    smoothing_alpha            REAL DEFAULT 0.2,  -- 指数平滑系数 α

    enable_deadband            BOOLEAN DEFAULT 0,
    deadband_value             REAL,              -- 死区值（绝对值）

    -- BlockClass 偏好（用于自适应升级）
    preferred_block_class INTEGER DEFAULT 1,     -- 1=RAW16K, 2=RAW64K, 3=RAW256K

    -- 元数据
    created_ts_us       INTEGER NOT NULL,
    updated_ts_us       INTEGER NOT NULL,

    -- 约束
    CHECK (value_type >= 1 AND value_type <= 4),
    CHECK (preferred_encoding >= 0 AND preferred_encoding <= 7),
    CHECK (low_extreme IS NULL OR high_extreme IS NULL OR low_extreme < high_extreme)
);

-- 索引
CREATE INDEX IF NOT EXISTS idx_tags_name ON tags(tag_name);
CREATE INDEX IF NOT EXISTS idx_tags_encoding ON tags(preferred_encoding);
```

**影响**：
- 新增核心表，`blocks` 表的 `tag_id` 引用此表
- 压缩配置作为 Tag 属性，避免额外 JOIN
- **立即添加，补全缺失的元数据基础设施**

---

#### **改造 3：ContainerHeader 增加 Archive 层级字段**

**当前问题**：
- `ContainerHeader` 没有 Archive 层级概念
- 无法支持多分辨率 Archive（高频/低频）

**改造方案**：
```cpp
enum ArchiveLevel : uint8_t {
    ARCHIVE_RAW          = 0,  // 原始高频数据
    ARCHIVE_RESAMPLED_1M = 1,  // 1 分钟重采样
    ARCHIVE_RESAMPLED_1H = 2,  // 1 小时重采样
    ARCHIVE_AGGREGATED   = 3,  // 聚合统计数据
    ARCHIVE_RESERVED_4   = 4,  // 预留
    ARCHIVE_RESERVED_5   = 5,  // 预留
};

struct ContainerHeaderV12 {
    char     magic[8];
    uint16_t version;
    uint16_t header_size;

    uint8_t  db_instance_id[16];

    uint8_t  layout;             // RAW_FIXED / COMPACT_VAR
    uint8_t  capacity_type;      // DYNAMIC / FIXED
    uint8_t  archive_level;      // ⭐ 新增：Archive 层级
    uint8_t  reserved_u8;

    uint16_t flags;
    uint16_t reserved_u16;

    uint64_t capacity_extents;
    int64_t  created_ts_us;

    // RAW 固定参数
    uint8_t  raw_block_class;
    uint8_t  reserved8[7];

    uint32_t chunk_size_extents;
    uint32_t block_size_extents;

    // ⭐ 新增：Archive 参数
    uint32_t resampling_interval_us;  // 重采样间隔（微秒）
    uint32_t reserved_archive_1;      // 预留

    uint32_t header_crc32;
    uint32_t reserved0;

    uint8_t  payload[/* ... */];
};
```

**影响**：
- `ContainerHeader` 仍保持 16KB，payload 区域略微减少
- **立即修改，支持未来多分辨率 Archive**

---

### 2.2 中期规划（Phase 4-5 实施）

#### **改造 4：实现 Swinging Door 压缩引擎**

**实施步骤**：
1. 实现 `SwingingDoorEncoder` 类
   - 输入：原始数据流 + 容差
   - 输出：线性段端点
2. 实现 `SwingingDoorDecoder` 类
   - 输入：线性段端点 + 查询时间范围
   - 输出：插值重建的数据点
3. 修改 `BlockWriter`
   - 根据 Tag 配置选择编码器
   - 写入编码后的数据
4. 修改 `BlockReader`
   - 根据 `encoding_type` 选择解码器
   - 透明插值返回数据

**接口设计**：
```cpp
class SwingingDoorEncoder {
public:
    struct Segment {
        int64_t start_ts_us;
        int64_t end_ts_us;
        float   start_value;
        float   end_value;
        uint8_t quality;
    };

    SwingingDoorEncoder(float tolerance, float compression_factor);

    // 输入新数据点
    void push(int64_t ts_us, float value, uint8_t quality);

    // 输出已确定的线性段
    std::vector<Segment> getSegments();

    // 强制 flush（block seal 时）
    Segment flush();
};

class SwingingDoorDecoder {
public:
    // 从线性段重建数据点
    std::vector<Record> interpolate(
        const std::vector<Segment>& segments,
        int64_t start_ts_us,
        int64_t end_ts_us,
        int64_t interval_us
    );
};
```

---

#### **改造 5：实现 16-bit 量化编码器**

**实施步骤**：
1. 实现 `QuantizedEncoder`
   - 输入：float 值 + low/high extreme
   - 输出：uint16_t
2. 实现 `QuantizedDecoder`
   - 输入：uint16_t + low/high extreme
   - 输出：float 值
3. 集成到 `BlockWriter/BlockReader`

**接口设计**：
```cpp
class QuantizedEncoder {
public:
    QuantizedEncoder(float low_extreme, float high_extreme);

    uint16_t encode(float value) const;
    float decode(uint16_t quantized) const;

private:
    float low_;
    float high_;
    float scale_;  // (high - low) / 65535
};
```

---

#### **改造 6：实现多分辨率 Archive 管理**

**实施步骤**：
1. 修改 SQLite 元数据
   - `containers` 表增加 `archive_level` 字段
   - 允许同一时间范围的多个 Container
2. 实现 `ArchiveManager` 类
   - 管理多个 Archive 的生命周期
   - 查询时自动选择最佳 Archive
3. 实现 `ResamplingEngine`
   - 定期从高频 Archive 重采样到低频 Archive
   - 支持多种聚合方式（avg/min/max/first/last）

**查询路由逻辑**：
```cpp
class ArchiveManager {
public:
    // 查询路由：根据时间范围和期望分辨率选择 Archive
    std::vector<Container*> selectArchives(
        int64_t start_ts_us,
        int64_t end_ts_us,
        int64_t desired_resolution_us
    ) {
        // 优先选择分辨率更高的 Archive
        // 示例：
        // - 查询最近 1 小时 → 选择 ARCHIVE_RAW
        // - 查询 3 年前数据 → 选择 ARCHIVE_RESAMPLED_1H
    }
};
```

---

### 2.3 长期规划（Phase 6+）

#### **改造 7：实现质量加权聚合**

**目标**：
- 在 AVG/SUM/WEIGHTED_AVG 计算时使用质量权重
- 支持置信度传播（插值/降采样时降低置信度）

**实施步骤**：
1. 扩展 `quality` 字段语义
   - 当前：2 bit 主质量 + 6 bit 自定义
   - 未来：可映射为 0-100 的置信度
2. 实现 `QualityWeightedAggregator`
   - 计算加权平均值
   - 考虑质量因子

**公式**：
```
weighted_avg = Σ(value_i * quality_i) / Σ(quality_i)
```

---

#### **改造 8：实现预处理管道（Ingestion Pipeline）**

**目标**：
- 支持配置化的数据预处理
- 毛刺剔除、平滑、死区等

**架构设计**：
```cpp
class IngestionPipeline {
public:
    // 注册预处理器
    void addProcessor(std::unique_ptr<DataProcessor> processor);

    // 处理输入数据
    std::vector<Record> process(const std::vector<Record>& input);
};

// 预处理器接口
class DataProcessor {
public:
    virtual std::vector<Record> process(const std::vector<Record>& input) = 0;
};

// 具体实现
class GrossErrorRemover : public DataProcessor { /* ... */ };
class ExponentialSmoother : public DataProcessor { /* ... */ };
class DeadbandGating : public DataProcessor { /* ... */ };
```

---

## 3. 实施优先级与里程碑

### Phase 3（当前）- 立即改造
- ✅ **改造 1**：`BlockDirEntry` 增加 `encoding_type` 字段
- ✅ **改造 2**：SQLite 增加 `tag_compression_config` 表
- ✅ **改造 3**：`ContainerHeader` 增加 `archive_level` 字段

**关键原因**：这些是**结构性修改**，必须在写入路径实现前完成，否则后续需要数据迁移。

---

### Phase 4-5（中期）- 功能增强
- 🔄 **改造 4**：实现 Swinging Door 压缩
- 🔄 **改造 5**：实现 16-bit 量化
- 🔄 **改造 6**：实现多分辨率 Archive

**关键原因**：核心压缩能力，显著提升存储效率。

---

### Phase 6+（长期）- 高级特性
- 📋 **改造 7**：质量加权聚合
- 📋 **改造 8**：预处理管道

**关键原因**：锦上添花的特性，不影响核心功能。

---

## 4. 兼容性保证策略

### 4.1 向后兼容性
- 所有新增字段使用 `reserved` 字段替换，不改变结构体大小
- 新增 `encoding_type = 0` 表示 RAW（无压缩），保持与现有逻辑兼容
- SQLite 表使用 `DEFAULT` 值，保证旧数据查询正常

### 4.2 渐进式演进
- Phase 3：仅增加字段，不改变行为（`encoding_type` 默认为 0）
- Phase 4：开始支持新编码，与 RAW 共存
- Phase 5：逐步迁移高价值 Tag 到压缩编码

### 4.3 可配置性
- 所有压缩/编码策略按 Tag 配置
- 支持逐个 Tag 启用/禁用
- 保留 RAW 格式作为 fallback

---

## 5. 总结与建议

### 5.1 核心价值
PHD 的设计哲学给 xTdb 最大的启示是：
1. **压缩是线性段，而非点**（Swinging Door）
2. **工程容差是压缩的核心参数**（按 Tag 配置）
3. **多分辨率是长期存储的必然选择**（分层 Archive）
4. **质量信息必须参与计算语义**（置信度加权）

### 5.2 立即行动项（Phase 3 之前）
**必须立即修改以下结构**，否则后续会面临数据格式迁移：

```cpp
// 1. BlockDirEntryV16 增加 encoding_type
struct BlockDirEntryV16 {
    // ...
    uint8_t  encoding_type;      // ⭐ 新增
    uint32_t encoding_param1;    // ⭐ 新增
    uint32_t encoding_param2;    // ⭐ 新增
};

// 2. ContainerHeaderV12 增加 archive_level
struct ContainerHeaderV12 {
    // ...
    uint8_t  archive_level;              // ⭐ 新增
    uint32_t resampling_interval_us;     // ⭐ 新增
};

// 3. SQLite 增加 tags 表（含压缩配置）
CREATE TABLE IF NOT EXISTS tags (
    tag_id              INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_name            TEXT NOT NULL UNIQUE,
    value_type          INTEGER NOT NULL,

    -- 压缩配置
    preferred_encoding  INTEGER DEFAULT 0,
    tolerance           REAL,
    low_extreme         REAL,
    high_extreme        REAL,
    /* ... */
);
```

### 5.3 投资回报
- **Swinging Door**：压缩比 10:1 至 100:1（平稳 PV）
- **16-bit 量化**：存储减少 50%-75%
- **多分辨率 Archive**：长期查询加速 10x-100x

**建议**：立即进行 Phase 3 的结构性改造，为未来留出空间。

---

*文档结束*
