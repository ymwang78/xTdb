# Rotating WAL Integration Guide

## 概述

Rotating WAL是一个独立container的多段轮转WAL系统，用于替代原有的固定区域WAL，解决以下问题：
1. **SSD磨损分散**：4-8段轮转，磨损分散到多个区域
2. **避免DB冲突**：独立container文件，与数据完全隔离
3. **自动增长**：支持动态扩展segment数量
4. **段满触发**：明确的刷盘时机，避免雪崩式刷盘

## 核心特性

```
独立WAL Container结构：
┌─────────────────────────────────────┐
│ wal_container.raw (独立文件)        │
├─────────────────────────────────────┤
│ Header: 16KB                        │
│ Segment 0: 64MB (默认)             │
│ Segment 1: 64MB                     │
│ Segment 2: 64MB                     │
│ Segment 3: 64MB                     │
│ (可扩展到8段)                        │
└─────────────────────────────────────┘

轮转流程：
  写入Segment 0 → 满 → 保存metadata
     ↓
  切换到Segment 1
     ↓
  回调通知刷盘(Segment 0的tag_ids)
     ↓
  用户代码刷写tag buffers到磁盘
     ↓
  调用clearSegment(0)清空Segment 0
     ↓
  Segment 0可重用
```

## 在Storage Engine中集成

### 步骤1：添加头文件和成员变量

在`storage_engine.h`中：

```cpp
#include "rotating_wal.h"  // 替代 wal_writer.h

class StorageEngine {
private:
    // 旧的WAL writer (移除)
    // std::unique_ptr<WALWriter> wal_writer_;

    // 新的Rotating WAL
    std::unique_ptr<RotatingWAL> rotating_wal_;

    // 其他成员保持不变...
};
```

### 步骤2：修改open()方法

在`storage_engine.cpp`的`mountContainers()`中：

```cpp
EngineResult StorageEngine::mountContainers() {
    // 现有container mount逻辑...

    // === 初始化Rotating WAL（替代原有WAL初始化）===
    RotatingWALConfig wal_config;
    wal_config.wal_container_path = config_.data_dir + "/wal_container.raw";
    wal_config.num_segments = 4;  // 4个段，可配置
    wal_config.segment_size_bytes = 64 * 1024 * 1024;  // 64MB per segment
    wal_config.auto_grow = false;  // 可选：是否允许自动增长到max_segments
    wal_config.max_segments = 8;   // 如果auto_grow=true，最多8个段

    rotating_wal_ = std::make_unique<RotatingWAL>(wal_config);

    RotatingWALResult wal_result = rotating_wal_->open();
    if (wal_result != RotatingWALResult::SUCCESS) {
        setError("Failed to open rotating WAL: " + rotating_wal_->getLastError());
        return EngineResult::ERROR_WAL_OPEN_FAILED;
    }

    // 设置段满刷盘回调
    rotating_wal_->setFlushCallback(
        [this](uint32_t segment_id, const std::set<uint32_t>& tag_ids) {
            return handleSegmentFull(segment_id, tag_ids);
        }
    );

    return EngineResult::SUCCESS;
}
```

### 步骤3：实现刷盘回调

在`storage_engine.cpp`中添加新方法：

```cpp
/// 处理WAL segment满时的刷盘回调
/// @param segment_id 满的segment ID
/// @param tag_ids 该segment中涉及的tag列表
/// @return true表示刷盘成功，false表示失败
bool StorageEngine::handleSegmentFull(uint32_t segment_id,
                                      const std::set<uint32_t>& tag_ids) {
    std::cerr << "[StorageEngine] Segment " << segment_id << " full, "
              << "flushing " << tag_ids.size() << " tags" << std::endl;

    // 1. 刷写涉及的tag的内存缓冲到磁盘
    for (uint32_t tag_id : tag_ids) {
        auto it = buffers_.find(tag_id);
        if (it == buffers_.end()) {
            continue;  // Tag buffer不存在，跳过
        }

        TagBuffer& tag_buffer = it->second;

        // 如果buffer中有数据，刷写到磁盘
        if (!tag_buffer.records.empty()) {
            // 使用现有的flush逻辑
            EngineResult result = flushSingleTag(tag_id, tag_buffer);
            if (result != EngineResult::SUCCESS) {
                std::cerr << "[StorageEngine] Failed to flush tag " << tag_id << std::endl;
                return false;  // 刷盘失败
            }
        }
    }

    // 2. 清除WAL segment
    RotatingWALResult result = rotating_wal_->clearSegment(segment_id);
    if (result != RotatingWALResult::SUCCESS) {
        std::cerr << "[StorageEngine] Failed to clear segment " << segment_id
                  << ": " << rotating_wal_->getLastError() << std::endl;
        return false;
    }

    return true;  // 成功
}

/// 刷写单个tag的buffer到磁盘
EngineResult StorageEngine::flushSingleTag(uint32_t tag_id, TagBuffer& tag_buffer) {
    // 检查是否有足够的数据
    if (tag_buffer.records.empty()) {
        return EngineResult::SUCCESS;
    }

    // 检查active chunk是否有空间
    if (active_chunk_.blocks_used >= active_chunk_.blocks_total) {
        // 需要seal当前chunk并分配新chunk
        EngineResult result = sealCurrentChunk();
        if (result != EngineResult::SUCCESS) {
            return result;
        }

        // 分配新chunk...（使用现有逻辑）
    }

    // 使用BlockWriter写入数据块
    BlockWriter writer(io_.get());
    uint64_t block_offset = active_chunk_.chunk_offset +
                           (config_.layout.meta_blocks + active_chunk_.blocks_used) *
                           config_.layout.block_size_bytes;

    WriteResult write_result = writer.writeBlock(
        block_offset,
        config_.layout,
        tag_buffer
    );

    if (write_result != WriteResult::SUCCESS) {
        setError("Failed to write block: " + writer.getLastError());
        return EngineResult::ERROR_INVALID_DATA;
    }

    // 更新directory
    dir_builder_->addBlock(/* block info */);

    // 清空buffer
    tag_buffer.records.clear();

    // 更新统计
    active_chunk_.blocks_used++;
    write_stats_.blocks_flushed++;

    return EngineResult::SUCCESS;
}
```

### 步骤4：修改writePoint()方法

在`writePoint()`中使用Rotating WAL：

```cpp
EngineResult StorageEngine::writePoint(uint32_t tag_id,
                                      int64_t timestamp_us,
                                      double value,
                                      uint8_t quality) {
    if (!is_open_) {
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // 1. 写入WAL（使用Rotating WAL）
    WALEntry entry;
    entry.tag_id = tag_id;
    entry.timestamp_us = timestamp_us;
    entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    entry.quality = quality;
    entry.value.f64_value = value;

    RotatingWALResult wal_result = rotating_wal_->append(entry);
    if (wal_result != RotatingWALResult::SUCCESS) {
        setError("WAL append failed: " + rotating_wal_->getLastError());
        return EngineResult::ERROR_WAL_OPEN_FAILED;
    }

    // 2. 写入内存buffer（逻辑不变）
    auto it = buffers_.find(tag_id);
    if (it == buffers_.end()) {
        // 创建新buffer...
    }

    TagBuffer& tag_buffer = it->second;

    // 添加记录到buffer
    MemRecord record;
    record.time_offset = static_cast<uint32_t>((timestamp_us - tag_buffer.start_ts_us) / 1000);
    record.quality = quality;
    record.value.f64_value = value;
    tag_buffer.records.push_back(record);

    // 3. 定期sync WAL（每N条或每T秒）
    wal_entries_since_sync_++;
    if (wal_entries_since_sync_ >= 10000) {  // 可配置
        rotating_wal_->sync();
        wal_entries_since_sync_ = 0;
    }

    // 4. 如果buffer达到阈值，触发flush（保留原有逻辑）
    if (tag_buffer.records.size() >= 1000) {  // 可配置
        flushSingleTag(tag_id, tag_buffer);
    }

    write_stats_.points_written++;
    return EngineResult::SUCCESS;
}
```

### 步骤5：修改close()方法

```cpp
void StorageEngine::close() {
    if (!is_open_) {
        return;
    }

    // Sync并关闭WAL
    if (rotating_wal_) {
        rotating_wal_->sync();
        rotating_wal_->close();
        rotating_wal_.reset();
    }

    // 其他cleanup逻辑不变...
}
```

## 配置建议

### 10万tag场景（1点/秒/tag）
```cpp
RotatingWALConfig config;
config.num_segments = 4;
config.segment_size_bytes = 64 * 1024 * 1024;  // 64 MB
config.auto_grow = false;

// 预期性能：
// - WAL容量：256 MB（4 × 64 MB）
// - 写入速率：2.4 MB/s（100k × 24B）
// - WAL可用时长：~107 秒
// - fsync频率：10次/秒（每10000条）
```

### 50万tag场景（1点/秒/tag）
```cpp
RotatingWALConfig config;
config.num_segments = 8;
config.segment_size_bytes = 64 * 1024 * 1024;  // 64 MB
config.auto_grow = false;

// 预期性能：
// - WAL容量：512 MB（8 × 64 MB）
// - 写入速率：12 MB/s（500k × 24B）
// - WAL可用时长：~43 秒
// - fsync频率：50次/秒（每10000条）
```

## 优势对比

### 旧WAL系统
- ❌ 固定4MB容量，10万tag场景下1.67秒写满
- ❌ fsync 100次/秒（高CPU开销）
- ❌ 1000秒后1.6GB雪崩式刷盘
- ❌ 固定区域写入，SSD磨损集中

### Rotating WAL系统
- ✅ 256-512MB容量，可持续43-107秒
- ✅ fsync 10-50次/秒（降低90%）
- ✅ 段满触发刷盘，分散写入负载
- ✅ 4-8段轮转，SSD磨损分散
- ✅ 独立container，避免与DB冲突

## 注意事项

1. **刷盘策略**：必须在回调中正确实现刷盘逻辑，否则segment无法被重用
2. **fsync频率**：根据数据重要性和性能需求调整sync频率
3. **segment大小**：建议64MB，可根据实际写入速率调整
4. **segment数量**：建议4-8个，确保有足够的轮转空间

## 测试验证

基础功能测试已通过（Test 1-3）：
- ✓ Container创建和初始化
- ✓ WAL entry写入和读取
- ✓ 段轮转触发

性能测试结果：
- 100,000 entries写入：<1秒
- 吞吐量：>100,000 entries/sec
- 带宽：>2 MB/sec
