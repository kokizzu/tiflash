// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/CurrentMetrics.h>
#include <Core/Defines.h>
#include <DataStreams/ConcatBlockInputStream.h>
#include <DataStreams/OneBlockInputStream.h>
#include <Interpreters/Context.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileDataProvider.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileTinyLocalIndexWriter.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/DeltaMergeStore.h>
#include <Storages/DeltaMerge/File/DMFileBlockOutputStream.h>
#include <Storages/DeltaMerge/File/DMFileLocalIndexWriter.h>
#include <Storages/DeltaMerge/Index/LocalIndexInfo_fwd.h>
#include <Storages/DeltaMerge/Segment.h>
#include <Storages/DeltaMerge/Segment_fwd.h>
#include <Storages/DeltaMerge/StoragePool/StoragePool.h>
#include <Storages/DeltaMerge/WriteBatchesImpl.h>
#include <Storages/DeltaMerge/tests/DMTestEnv.h>
#include <Storages/DeltaMerge/tests/gtest_segment_test_basic.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/Page/PageDefinesBase.h>
#include <TestUtils/InputStreamTestUtils.h>
#include <TestUtils/TiFlashStorageTestBasic.h>
#include <TestUtils/TiFlashTestBasic.h>
#include <TiDB/Schema/TiDB.h>

#include <magic_enum.hpp>

namespace CurrentMetrics
{
extern const Metric DT_SnapshotOfReadRaw;
extern const Metric DT_SnapshotOfRead;
extern const Metric DT_SnapshotOfBitmapFilter;
} // namespace CurrentMetrics

namespace DB::DM
{
extern DMFilePtr writeIntoNewDMFile(
    DMContext & dm_context,
    const ColumnDefinesPtr & schema_snap,
    const BlockInputStreamPtr & input_stream,
    UInt64 file_id,
    const String & parent_path);
}

namespace DB::DM::tests
{

namespace
{

// a + b
bool isSumOverflow(Int64 a, Int64 b)
{
    return (b > 0 && a > std::numeric_limits<Int64>::max() - b) || (b < 0 && a < std::numeric_limits<Int64>::min() - b);
}

// a - b
auto isDiffOverflow(Int64 a, Int64 b)
{
    assert(a > b);
    if (b < 0)
        return a > std::numeric_limits<Int64>::max() + b;
    else
        return false;
}
} // namespace

void SegmentTestBasic::reloadWithOptions(SegmentTestOptions config)
{
    {
        auto const seed = std::random_device{}();
        random = std::mt19937{seed};
    }

    logger = Logger::get();
    logger_op = Logger::get("SegmentTestOperation");

    TiFlashStorageTestBasic::SetUp();
    options = config;
    table_columns = std::make_shared<ColumnDefines>();

    root_segment = reload(config.is_common_handle, nullptr, std::move(config.db_settings));
    ASSERT_EQ(root_segment->segmentId(), DELTA_MERGE_FIRST_SEGMENT_ID);
    segments.clear();
    segments[DELTA_MERGE_FIRST_SEGMENT_ID] = root_segment;
}

size_t SegmentTestBasic::getSegmentRowNumWithoutMVCC(PageIdU64 segment_id)
{
    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    auto in = segment->getInputStreamModeRaw(*dm_context, *tableColumns());
    return getInputStreamNRows(in);
}

size_t SegmentTestBasic::getSegmentRowNum(PageIdU64 segment_id)
{
    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    auto snap = segment->createSnapshot(*dm_context, /*for_update*/ false, CurrentMetrics::DT_SnapshotOfRead);
    auto in = segment->getInputStreamModeNormal(
        *dm_context,
        *tableColumns(),
        snap,
        {segment->getRowKeyRange()},
        {},
        std::numeric_limits<UInt64>::max(),
        DEFAULT_BLOCK_SIZE);
    return getInputStreamNRows(in);
}

bool SegmentTestBasic::isSegmentDefinitelyEmpty(PageIdU64 segment_id)
{
    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    auto snapshot = segment->createSnapshot(*dm_context, /* for_update */ true, CurrentMetrics::DT_SnapshotOfReadRaw);
    RUNTIME_CHECK(snapshot != nullptr);
    return segment->isDefinitelyEmpty(*dm_context, snapshot);
}

std::optional<PageIdU64> SegmentTestBasic::splitSegment(
    PageIdU64 segment_id,
    Segment::SplitMode split_mode,
    bool check_rows)
{
    LOG_INFO(logger_op, "splitSegment, segment_id={} split_mode={}", segment_id, magic_enum::enum_name(split_mode));

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto origin_segment = segments[segment_id];
    size_t origin_segment_row_num = getSegmentRowNum(segment_id);

    LOG_DEBUG(
        logger,
        "begin split, segment_id={} split_mode={} rows={}",
        segment_id,
        magic_enum::enum_name(split_mode),
        origin_segment_row_num);

    auto [left, right] = origin_segment->split(
        *dm_context,
        tableColumns(),
        /* use a calculated split point */ std::nullopt,
        split_mode);
    if (!left && !right)
    {
        LOG_DEBUG(
            logger,
            "split not succeeded, segment_id={} split_mode={} rows={}",
            segment_id,
            magic_enum::enum_name(split_mode),
            origin_segment_row_num);
        return std::nullopt;
    }

    RUNTIME_CHECK(left && right);
    RUNTIME_CHECK(left->segmentId() == segment_id, segment_id, left->info());
    segments[left->segmentId()] = left; // The left segment is updated
    segments[right->segmentId()] = right;

    auto left_rows = getSegmentRowNum(segment_id);
    auto right_rows = getSegmentRowNum(right->segmentId());

    if (check_rows)
        EXPECT_EQ(origin_segment_row_num, left_rows + right_rows);

    LOG_DEBUG(
        logger,
        "split finish, left_id={} left_rows={} right_id={} right_rows={}",
        left->segmentId(),
        left_rows,
        right->segmentId(),
        right_rows);
    operation_statistics[fmt::format("split{}", magic_enum::enum_name(split_mode))]++;

    return right->segmentId();
}

std::optional<PageIdU64> SegmentTestBasic::splitSegmentAt(
    PageIdU64 segment_id,
    Int64 split_at,
    Segment::SplitMode split_mode,
    bool check_rows)
{
    LOG_INFO(
        logger_op,
        "splitSegmentAt, segment_id={} split_at={} split_mode={}",
        segment_id,
        split_at,
        magic_enum::enum_name(split_mode));

    RowKeyValue split_at_key;
    if (options.is_common_handle)
    {
        WriteBufferFromOwnString ss;
        ::DB::EncodeUInt(static_cast<UInt8>(TiDB::CodecFlagInt), ss);
        ::DB::EncodeInt64(split_at, ss);
        split_at_key = RowKeyValue{true, std::make_shared<String>(ss.releaseStr()), split_at};
    }
    else
    {
        split_at_key = RowKeyValue::fromIntHandle(split_at);
    }

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto origin_segment = segments[segment_id];
    size_t origin_segment_row_num = getSegmentRowNum(segment_id);

    LOG_DEBUG(
        logger,
        "begin splitAt, segment_id={} split_at={} split_at_key={} split_mode={} rows={}",
        segment_id,
        split_at,
        split_at_key.toDebugString(),
        magic_enum::enum_name(split_mode),
        origin_segment_row_num);

    auto [left, right] = origin_segment->split(*dm_context, tableColumns(), split_at_key, split_mode);
    if (!left && !right)
    {
        LOG_DEBUG(
            logger,
            "splitAt not succeeded, segment_id={} split_at={} split_mode={} rows={}",
            segment_id,
            split_at,
            magic_enum::enum_name(split_mode),
            origin_segment_row_num);
        return std::nullopt;
    }

    RUNTIME_CHECK(left && right);
    RUNTIME_CHECK(left->segmentId() == segment_id, segment_id, left->info());
    segments[left->segmentId()] = left; // The left segment is updated
    segments[right->segmentId()] = right;

    auto left_rows = getSegmentRowNum(segment_id);
    auto right_rows = getSegmentRowNum(right->segmentId());

    if (check_rows)
        EXPECT_EQ(origin_segment_row_num, left_rows + right_rows);

    LOG_DEBUG(
        logger,
        "splitAt finish, left_id={} left_rows={} right_id={} right_rows={}",
        left->segmentId(),
        left_rows,
        right->segmentId(),
        right_rows);
    operation_statistics[fmt::format("splitAt{}", magic_enum::enum_name(split_mode))]++;

    return right->segmentId();
}

void SegmentTestBasic::mergeSegment(const PageIdU64s & segments_id, bool check_rows)
{
    LOG_INFO(logger_op, "mergeSegment, segments=[{}]", fmt::join(segments_id, ","));

    RUNTIME_CHECK(segments_id.size() >= 2, segments_id.size());

    std::vector<SegmentPtr> segments_to_merge;
    std::vector<size_t> segments_rows;
    size_t merged_rows = 0;
    segments_to_merge.reserve(segments_id.size());
    segments_rows.reserve(segments_id.size());

    for (const auto segment_id : segments_id)
    {
        RUNTIME_CHECK(segments.find(segment_id) != segments.end());
        segments_to_merge.emplace_back(segments[segment_id]);

        auto rows = getSegmentRowNum(segment_id);
        segments_rows.emplace_back(rows);
        merged_rows += rows;
    }

    LOG_DEBUG(
        logger,
        "begin merge, segments=[{}] each_rows=[{}]",
        fmt::join(segments_id, ","),
        fmt::join(segments_rows, ","));

    SegmentPtr merged_segment = Segment::merge(*dm_context, tableColumns(), segments_to_merge);
    if (!merged_segment)
    {
        LOG_DEBUG(
            logger,
            "merge not succeeded, segments=[{}] each_rows=[{}]",
            fmt::join(segments_id, ","),
            fmt::join(segments_rows, ","));
        return;
    }

    for (const auto segment_id : segments_id)
        segments.erase(segments.find(segment_id));
    segments[merged_segment->segmentId()] = merged_segment;

    if (check_rows)
        EXPECT_EQ(getSegmentRowNum(merged_segment->segmentId()), merged_rows);

    LOG_DEBUG(
        logger,
        "merge finish, merged_segment_id={} merge_from_segments=[{}] merged_rows={}",
        merged_segment->segmentId(),
        fmt::join(segments_id, ","),
        merged_rows);
    if (segments_id.size() > 2)
        operation_statistics["mergeMultiple"]++;
    else
        operation_statistics["mergeTwo"]++;
}

void SegmentTestBasic::mergeSegmentDelta(PageIdU64 segment_id, bool check_rows, std::optional<size_t> pack_size)
{
    LOG_INFO(logger_op, "mergeSegmentDelta, segment_id={}, pack_size={}", segment_id, pack_size);

    const size_t initial_pack_rows = db_context->getSettingsRef().dt_segment_stable_pack_rows;
    if (pack_size)
    {
        db_context->getSettingsRef().dt_segment_stable_pack_rows = *pack_size;
        reloadDMContext();
        ASSERT_EQ(dm_context->stable_pack_rows, *pack_size);
    }
    SCOPE_EXIT({
        if (initial_pack_rows != db_context->getSettingsRef().dt_segment_stable_pack_rows)
        {
            db_context->getSettingsRef().dt_segment_stable_pack_rows = initial_pack_rows;
            reloadDMContext();
        }
    });

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNum(segment_id);
    SegmentPtr merged_segment = segment->mergeDelta(*dm_context, tableColumns());
    segments[merged_segment->segmentId()] = merged_segment;
    if (check_rows)
    {
        EXPECT_EQ(getSegmentRowNum(merged_segment->segmentId()), segment_row_num);
    }
    operation_statistics["mergeDelta"]++;
}

void SegmentTestBasic::flushSegmentCache(PageIdU64 segment_id)
{
    LOG_INFO(logger_op, "flushSegmentCache, segment_id={}", segment_id);

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNum(segment_id);
    segment->flushCache(*dm_context);
    EXPECT_EQ(getSegmentRowNum(segment_id), segment_row_num);
    operation_statistics["flush"]++;
}

void SegmentTestBasic::compactSegmentDelta(PageIdU64 segment_id)
{
    LOG_INFO(logger_op, "compactSegmentDelta, segment_id={}", segment_id);

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNum(segment_id);
    segment->compactDelta(*dm_context);
    EXPECT_EQ(getSegmentRowNum(segment_id), segment_row_num);
    operation_statistics["compact"]++;
}

std::pair<Int64, Int64> SegmentTestBasic::getSegmentKeyRange(PageIdU64 segment_id) const
{
    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    const auto & segment = segments.find(segment_id)->second;

    Int64 start_key, end_key;
    if (!options.is_common_handle)
    {
        start_key = segment->getRowKeyRange().getStart().int_value;
        end_key = segment->getRowKeyRange().getEnd().int_value;
        return {start_key, end_key};
    }

    const auto & range = segment->getRowKeyRange();
    if (range.isStartInfinite())
    {
        start_key = std::numeric_limits<Int64>::min();
    }
    else
    {
        EXPECT_EQ(range.getStart().data[0], TiDB::CodecFlagInt);
        size_t cursor = 1;
        start_key = DecodeInt64(cursor, String(range.getStart().data, range.getStart().size));
    }
    if (range.isEndInfinite())
    {
        end_key = std::numeric_limits<Int64>::max();
    }
    else
    {
        EXPECT_EQ(range.getEnd().data[0], TiDB::CodecFlagInt);
        size_t cursor = 1;
        end_key = DecodeInt64(cursor, String(range.getEnd().data, range.getEnd().size));
    }
    return {start_key, end_key};
}

Block SegmentTestBasic::prepareWriteBlockImpl(
    Int64 start_key,
    Int64 end_key,
    bool is_deleted,
    bool including_right_boundary,
    std::optional<UInt64> ts)
{
    RUNTIME_CHECK(start_key <= end_key, start_key, end_key);
    if (end_key == start_key && !including_right_boundary)
        return Block{};

    UInt64 v = ts.value_or(version + 1);
    version = std::max(v, version + 1); // Increase version
    return DMTestEnv::prepareSimpleWriteBlock(
        start_key, //
        end_key,
        false,
        v,
        DMTestEnv::pk_name,
        MutSup::extra_handle_id,
        options.is_common_handle ? MutSup::getExtraHandleColumnStringType() : MutSup::getExtraHandleColumnIntType(),
        options.is_common_handle,
        1,
        true,
        is_deleted,
        /*with_null_uint64*/ false,
        including_right_boundary);
}

Block SegmentTestBasic::prepareWriteBlock(
    Int64 start_key,
    Int64 end_key,
    bool is_deleted,
    bool including_right_boundary,
    std::optional<UInt64> ts)
{
    return prepareWriteBlockImpl(start_key, end_key, is_deleted, including_right_boundary, ts);
}

Block sortvstackBlocks(std::vector<Block> && blocks)
{
    auto accumulated_block = vstackBlocks(std::move(blocks));

    SortDescription sort;
    sort.emplace_back(MutSup::extra_handle_column_name, 1, 0);
    sort.emplace_back(MutSup::version_column_name, 1, 0);
    stableSortBlock(accumulated_block, sort);

    return accumulated_block;
}

Block SegmentTestBasic::prepareWriteBlockInSegmentRange(
    PageIdU64 segment_id,
    Int64 total_write_rows,
    std::optional<Int64> write_start_key,
    bool is_deleted,
    std::optional<UInt64> ts)
{
    RUNTIME_CHECK(0 < total_write_rows, total_write_rows);

    // For example, write_start_key is int64_max and total_write_rows is 1,
    // We will generate block with one row with int64_max.
    auto is_including_right_boundary = [](Int64 write_start_key, Int64 total_write_rows) {
        return std::numeric_limits<Int64>::max() - total_write_rows + 1 == write_start_key;
    };

    auto seg = segments.find(segment_id);
    RUNTIME_CHECK(seg != segments.end());
    auto segment_range = seg->second->getRowKeyRange();
    auto [segment_start_key, segment_end_key] = getSegmentKeyRange(segment_id);
    bool including_right_boundary = false;
    if (write_start_key.has_value())
    {
        RUNTIME_CHECK_MSG(
            segment_start_key <= *write_start_key
                && (*write_start_key < segment_end_key || segment_range.isEndInfinite()),
            "write_start_key={} segment_range={}",
            *write_start_key,
            segment_range.toDebugString());

        including_right_boundary = is_including_right_boundary(*write_start_key, total_write_rows);
        RUNTIME_CHECK(
            including_right_boundary || !isSumOverflow(*write_start_key, total_write_rows),
            *write_start_key,
            total_write_rows);
    }
    else
    {
        auto segment_max_rows = isDiffOverflow(segment_end_key, segment_start_key)
            ? std::numeric_limits<Int64>::max() // UInt64 is more accurate, but Int64 is enough and for simplicity.
            : segment_end_key - segment_start_key;

        if (segment_max_rows == 0)
            return {};

        // When write start key is unspecified, we will:
        // A. If the segment is large enough, we randomly pick a write start key in the range.
        // B. If the segment is small, we write from the beginning.
        if (segment_max_rows > total_write_rows)
        {
            write_start_key = std::uniform_int_distribution<Int64>{
                segment_start_key,
                segment_end_key - static_cast<Int64>(total_write_rows)}(random);
        }
        else
        {
            write_start_key = segment_start_key;
        }
    }

    auto max_write_rows_each_round = static_cast<UInt64>(segment_end_key - *write_start_key);
    RUNTIME_CHECK(max_write_rows_each_round > 0);
    RUNTIME_CHECK(*write_start_key >= segment_start_key);

    std::vector<Block> blocks;

    // If the length of segment key range is larger than `write_rows`, then
    // write the new data with the same tso in one block.
    // Otherwise create multiple block with increasing tso until the `remain_row_num`
    // down to 0.
    UInt64 remaining_rows = total_write_rows;
    while (remaining_rows > 0)
    {
        UInt64 write_rows_this_round = std::min(remaining_rows, max_write_rows_each_round);
        RUNTIME_CHECK(write_rows_this_round > 0);
        Int64 write_end_key_this_round = *write_start_key + static_cast<Int64>(write_rows_this_round);
        RUNTIME_CHECK(write_end_key_this_round <= segment_end_key);
        Block block
            = prepareWriteBlock(*write_start_key, write_end_key_this_round, is_deleted, including_right_boundary, ts);
        blocks.emplace_back(block);
        remaining_rows -= write_rows_this_round + including_right_boundary;

        LOG_DEBUG(
            logger,
            "Prepared block for write, block_range=[{}, {}) (rows={}), total_rows_to_write={} remain_rows={}", //
            *write_start_key,
            write_end_key_this_round,
            write_rows_this_round,
            total_write_rows,
            remaining_rows);
    }

    return sortvstackBlocks(std::move(blocks));
}

void SegmentTestBasic::writeToCache(
    PageIdU64 segment_id,
    UInt64 write_rows,
    Int64 start_at,
    bool shuffle,
    std::optional<UInt64> ts)
{
    LOG_INFO(logger_op, "writeToCache, segment_id={} write_rows={}", segment_id, write_rows);

    if (write_rows == 0)
        return;

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNumWithoutMVCC(segment_id);
    auto [start_key, end_key] = getSegmentKeyRange(segment_id);
    LOG_DEBUG(
        logger,
        "write to segment, segment={} segment_rows={} start_key={} end_key={}",
        segment->info(),
        segment_row_num,
        start_key,
        end_key);

    auto block = prepareWriteBlockInSegmentRange(segment_id, write_rows, start_at, /* is_deleted */ false, ts);

    if (shuffle)
    {
        IColumn::Permutation perm(block.rows());
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), std::mt19937(std::random_device()()));
        for (auto & column : block)
            column.column = column.column->permute(perm, 0);
    }
    segment->writeToCache(*dm_context, block, 0, block.rows());

    EXPECT_EQ(getSegmentRowNumWithoutMVCC(segment_id), segment_row_num + write_rows);
    operation_statistics["write"]++;
}

void SegmentTestBasic::writeSegment(
    PageIdU64 segment_id,
    UInt64 write_rows,
    std::optional<Int64> start_at,
    bool shuffle,
    std::optional<UInt64> ts)
{
    LOG_INFO(logger_op, "writeSegment, segment_id={} write_rows={}", segment_id, write_rows);

    if (write_rows == 0)
        return;

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNumWithoutMVCC(segment_id);
    auto [start_key, end_key] = getSegmentKeyRange(segment_id);
    LOG_DEBUG(
        logger,
        "write to segment, segment={} segment_rows={} start_key={} end_key={}",
        segment->info(),
        segment_row_num,
        start_key,
        end_key);

    auto block = prepareWriteBlockInSegmentRange(segment_id, write_rows, start_at, /* is_deleted */ false, ts);

    if (shuffle)
    {
        IColumn::Permutation perm(block.rows());
        std::iota(perm.begin(), perm.end(), 0);
        std::shuffle(perm.begin(), perm.end(), std::mt19937(std::random_device()()));
        for (auto & column : block)
            column.column = column.column->permute(perm, 0);
    }
    segment->write(*dm_context, block, false);

    EXPECT_EQ(getSegmentRowNumWithoutMVCC(segment_id), segment_row_num + write_rows);
    operation_statistics["write"]++;
}

BlockInputStreamPtr SegmentTestBasic::getIngestDTFileInputStream(
    PageIdU64 segment_id,
    UInt64 write_rows,
    std::optional<Int64> start_at,
    std::optional<size_t> pack_size,
    bool check_range)
{
    if (pack_size)
        RUNTIME_CHECK(start_at.has_value());

    auto rows_per_block = pack_size ? *pack_size : DEFAULT_MERGE_BLOCK_SIZE;
    std::vector<BlockInputStreamPtr> streams;
    for (UInt64 written = 0; written < write_rows; written += rows_per_block)
    {
        rows_per_block = std::min(rows_per_block, write_rows - written);
        std::optional<Int64> start;
        if (start_at)
        {
            RUNTIME_CHECK(!isSumOverflow(*start_at, written), *start_at, written);
            start.emplace(*start_at + written);
        }

        if (check_range)
        {
            auto block = prepareWriteBlockInSegmentRange(segment_id, rows_per_block, start, /* is_deleted */ false);
            streams.push_back(std::make_shared<OneBlockInputStream>(std::move(block)));
        }
        else
        {
            Int64 start_key = start.value_or(0);
            bool overflow = std::numeric_limits<Int64>::max() - static_cast<Int64>(rows_per_block) < start_key;
            Int64 end_key = overflow ? std::numeric_limits<Int64>::max() : start_key + rows_per_block;
            // if overflow, write [start_key, end_key]
            // if not overflow, write [start_key, end_key)
            rows_per_block += overflow;
            auto block = prepareWriteBlock(start_key, end_key, /*is_deleted*/ false, overflow);
            streams.push_back(std::make_shared<OneBlockInputStream>(std::move(block)));
        }
    }
    return std::make_shared<ConcatBlockInputStream>(std::move(streams), "");
}

void SegmentTestBasic::ingestDTFileIntoDelta(
    PageIdU64 segment_id,
    UInt64 write_rows,
    std::optional<Int64> start_at,
    bool clear,
    std::optional<size_t> pack_size,
    bool check_range)
{
    LOG_INFO(logger_op, "ingestDTFileIntoDelta, segment_id={} write_rows={}", segment_id, write_rows);

    if (write_rows == 0)
        return;

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());

    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNumWithoutMVCC(segment_id);
    auto [start_key, end_key] = getSegmentKeyRange(segment_id);
    LOG_DEBUG(
        logger,
        "ingest to segment delta, segment={} segment_rows={} start_key={} end_key={}",
        segment->info(),
        segment_row_num,
        start_key,
        end_key);

    {
        auto input_stream = getIngestDTFileInputStream(segment_id, write_rows, start_at, pack_size, check_range);
        WriteBatches ingest_wbs(*dm_context->storage_pool, dm_context->getWriteLimiter());
        auto delegator = storage_path_pool->getStableDiskDelegator();
        auto parent_path = delegator.choosePath();
        auto file_id = storage_pool->newDataPageIdForDTFile(delegator, __PRETTY_FUNCTION__);
        auto dm_file = writeIntoNewDMFile(*dm_context, table_columns, input_stream, file_id, parent_path);
        ingest_wbs.data.putExternal(file_id, /* tag */ 0);
        ingest_wbs.writeLogAndData();
        delegator.addDTFile(file_id, dm_file->getBytesOnDisk(), parent_path);

        WriteBatches wbs(*dm_context->storage_pool, dm_context->getWriteLimiter());
        auto ref_id = storage_pool->newDataPageIdForDTFile(delegator, __PRETTY_FUNCTION__);
        wbs.data.putRefPage(ref_id, dm_file->pageId());
        auto ref_file = DMFile::restore(
            dm_context->global_context.getFileProvider(),
            file_id,
            ref_id,
            parent_path,
            DMFileMeta::ReadMode::all());
        wbs.writeLogAndData();
        ASSERT_TRUE(segment->ingestDataToDelta(
            *dm_context,
            segment->getRowKeyRange(),
            {ref_file},
            /* clear_data_in_range */ clear));

        ingest_wbs.rollbackWrittenLogAndData();
    }
    // If check_range is false, we may generate some data that range is larger than segment.
    if (check_range)
        EXPECT_EQ(getSegmentRowNumWithoutMVCC(segment_id), segment_row_num + write_rows);
    operation_statistics["ingest"]++;
}

void SegmentTestBasic::ingestDTFileByReplace(
    PageIdU64 segment_id,
    UInt64 write_rows,
    std::optional<Int64> start_at,
    bool clear)
{
    LOG_INFO(logger_op, "ingestDTFileByReplace, segment_id={} write_rows={}", segment_id, write_rows);

    if (write_rows == 0)
        return;

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());

    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNumWithoutMVCC(segment_id);
    auto [start_key, end_key] = getSegmentKeyRange(segment_id);
    LOG_DEBUG(
        logger,
        "ingest to segment delta, segment={} segment_rows={} start_key={} end_key={}",
        segment->info(),
        segment_row_num,
        start_key,
        end_key);

    {
        auto block = prepareWriteBlockInSegmentRange(segment_id, write_rows, start_at, /* is_deleted */ false);
        WriteBatches ingest_wbs(*dm_context->storage_pool, dm_context->getWriteLimiter());
        auto delegator = storage_path_pool->getStableDiskDelegator();
        auto parent_path = delegator.choosePath();
        auto file_id = storage_pool->newDataPageIdForDTFile(delegator, __PRETTY_FUNCTION__);
        auto input_stream = std::make_shared<OneBlockInputStream>(block);
        auto dm_file = writeIntoNewDMFile(*dm_context, table_columns, input_stream, file_id, parent_path);
        ingest_wbs.data.putExternal(file_id, /* tag */ 0);
        ingest_wbs.writeLogAndData();
        delegator.addDTFile(file_id, dm_file->getBytesOnDisk(), parent_path);

        WriteBatches wbs(*dm_context->storage_pool, dm_context->getWriteLimiter());
        auto ref_id = storage_pool->newDataPageIdForDTFile(delegator, __PRETTY_FUNCTION__);
        wbs.data.putRefPage(ref_id, dm_file->pageId());
        auto ref_file = DMFile::restore(
            dm_context->global_context.getFileProvider(),
            file_id,
            ref_id,
            parent_path,
            DMFileMeta::ReadMode::all());
        wbs.writeLogAndData();

        auto apply_result = segment->ingestDataForTest(*dm_context, ref_file, clear);

        ingest_wbs.rollbackWrittenLogAndData();

        if (apply_result.get() != segment.get())
        {
            operation_statistics["ingestByReplace_NewSegment"]++;
            const auto & new_segment = apply_result;
            segments[new_segment->segmentId()] = new_segment;
        }
        else if (apply_result.get() == segment.get())
        {
            operation_statistics["ingestByReplace_ReuseSegment"]++;
        }
        else
        {
            RUNTIME_CHECK(false);
        }
    }

    if (clear)
        EXPECT_EQ(getSegmentRowNumWithoutMVCC(segment_id), write_rows);
    else
        EXPECT_EQ(getSegmentRowNumWithoutMVCC(segment_id), segment_row_num + write_rows);
}

void SegmentTestBasic::writeSegmentWithDeletedPack(
    PageIdU64 segment_id,
    UInt64 write_rows,
    std::optional<Int64> start_at)
{
    LOG_INFO(logger_op, "writeSegmentWithDeletedPack, segment_id={} write_rows={}", segment_id, write_rows);

    if (write_rows == 0)
        return;

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    size_t segment_row_num = getSegmentRowNumWithoutMVCC(segment_id);
    auto [start_key, end_key] = getSegmentKeyRange(segment_id);
    LOG_DEBUG(
        logger,
        "write deleted pack to segment, segment={} segment_rows={} start_key={} end_key={}",
        segment->info(),
        segment_row_num,
        start_key,
        end_key);

    auto block = prepareWriteBlockInSegmentRange(segment_id, write_rows, start_at, /* is_deleted */ true);
    segment->write(*dm_context, block, false);

    EXPECT_EQ(getSegmentRowNumWithoutMVCC(segment_id), segment_row_num + write_rows);
    operation_statistics["writeDelete"]++;
}

void SegmentTestBasic::deleteRangeSegment(PageIdU64 segment_id)
{
    LOG_INFO(logger_op, "deleteRangeSegment, segment_id={}", segment_id);

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    segment->write(*dm_context, /*delete_range*/ segment->getRowKeyRange());
    EXPECT_EQ(getSegmentRowNum(segment_id), 0);
}

void SegmentTestBasic::replaceSegmentData(PageIdU64 segment_id, const Block & block, SegmentSnapshotPtr snapshot)
{
    // This function always create a new DTFile for the block.

    LOG_DEBUG(logger, "replace segment data using block, segment_id={} block_rows={}", segment_id, block.rows());

    auto delegator = storage_path_pool->getStableDiskDelegator();
    auto parent_path = delegator.choosePath();
    auto file_provider = db_context->getFileProvider();

    WriteBatches ingest_wbs(*dm_context->storage_pool, dm_context->getWriteLimiter());

    auto file_id = storage_pool->newDataPageIdForDTFile(delegator, __PRETTY_FUNCTION__);
    auto input_stream = std::make_shared<OneBlockInputStream>(block);
    auto dm_file = writeIntoNewDMFile(*dm_context, table_columns, input_stream, file_id, parent_path);

    ingest_wbs.data.putExternal(file_id, /* tag */ 0);
    ingest_wbs.writeLogAndData();

    delegator.addDTFile(file_id, dm_file->getBytesOnDisk(), parent_path);

    replaceSegmentData(segment_id, dm_file, snapshot);

    dm_file->enableGC();
}

void SegmentTestBasic::replaceSegmentData(PageIdU64 segment_id, const DMFilePtr & file, SegmentSnapshotPtr snapshot)
{
    LOG_INFO(
        logger_op,
        "replaceSegmentData, segment_id={} file_rows={} file=dmf_{}",
        segment_id,
        file->getRows(),
        file->fileId());

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    {
        auto lock = segment->mustGetUpdateLock();
        auto new_segment = segment->replaceData(lock, *dm_context, file, snapshot);
        if (new_segment != nullptr)
            segments[new_segment->segmentId()] = new_segment;
    }

    if (snapshot != nullptr)
        operation_statistics["replaceDataWithSnapshot"]++;
    else
        operation_statistics["replaceData"]++;
}

bool SegmentTestBasic::replaceSegmentStableData(PageIdU64 segment_id, const DMFilePtr & file)
{
    LOG_INFO(
        logger_op,
        "replaceSegmentStableData, segment_id={} file=dmf_{}(v={})",
        segment_id,
        file->fileId(),
        file->metaVersion());

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());

    bool success = false;
    auto segment = segments[segment_id];
    {
        auto lock = segment->mustGetUpdateLock();
        auto new_segment = segment->replaceStableMetaVersion(lock, *dm_context, {file});
        if (new_segment != nullptr)
        {
            segments[new_segment->segmentId()] = new_segment;
            success = true;
        }
    }

    operation_statistics["replaceStableData"]++;
    return success;
}

bool SegmentTestBasic::ensureSegmentStableLocalIndex(PageIdU64 segment_id, const LocalIndexInfosPtr & local_index_infos)
{
    LOG_INFO(logger_op, "EnsureSegmentStableLocalIndex, segment_id={}", segment_id);

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());

    bool success = false;
    auto segment = segments[segment_id];
    auto dm_files = segment->getStable()->getDMFiles();
    auto build_info = DMFileLocalIndexWriter::getLocalIndexBuildInfo(local_index_infos, dm_files);

    // Build index
    DMFileLocalIndexWriter iw(DMFileLocalIndexWriter::Options{
        .path_pool = storage_path_pool,
        .index_infos = build_info.indexes_to_build,
        .dm_files = dm_files,
        .dm_context = *dm_context,
    });
    auto new_dmfiles = iw.build();
    RUNTIME_CHECK(new_dmfiles.size() == 1);

    LOG_INFO(logger_op, "EnsureSegmentStableLocalIndex, build index done, segment_id={}", segment_id);

    // Replace stable data
    success = replaceSegmentStableData(segment_id, new_dmfiles[0]);

    operation_statistics["ensureStableLocalIndex"]++;
    return success;
}

bool SegmentTestBasic::ensureSegmentDeltaLocalIndex(PageIdU64 segment_id, const LocalIndexInfosPtr & local_index_infos)
{
    LOG_INFO(logger_op, "EnsureSegmentDeltaLocalIndex, segment_id={}", segment_id);

    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    auto delta = segment->delta;
    if (auto lock = delta->getLock(); !lock)
    {
        return false;
    }
    auto storage_snap = std::make_shared<StorageSnapshot>(*storage_pool, nullptr, "", true);
    auto data_from_storage_snap = ColumnFileDataProviderLocalStoragePool::create(storage_snap);
    auto persisted_files_snap = delta->getPersistedFileSet()->createSnapshot(data_from_storage_snap);
    WriteBatches wbs(*storage_pool, nullptr);
    // Build index
    ColumnFileTinyLocalIndexWriter iw(ColumnFileTinyLocalIndexWriter::Options{
        .storage_pool = storage_pool,
        .write_limiter = nullptr,
        .files = persisted_files_snap->getColumnFiles(),
        .data_provider = persisted_files_snap->getDataProvider(),
        .index_infos = local_index_infos,
        .wbs = wbs,
    });
    auto new_tiny_files = iw.build([] { return true; });

    ColumnFilePersisteds new_persisted_files;
    for (const auto & f : new_tiny_files)
    {
        new_persisted_files.push_back(f);
    }
    delta->getPersistedFileSet()->updatePersistedColumnFilesAfterAddingIndex(new_persisted_files, wbs);

    operation_statistics["ensureDeltaLocalIndex"]++;
    LOG_INFO(logger_op, "EnsureSegmentDeltaLocalIndex, build index done, segment_id={}", segment_id);

    return true;
}

bool SegmentTestBasic::areSegmentsSharingStable(const std::vector<PageIdU64> & segments_id) const
{
    RUNTIME_CHECK(segments_id.size() >= 2);
    for (auto segment_id : segments_id)
        RUNTIME_CHECK(segments.find(segment_id) != segments.end());

    auto base_stable = segments.find(segments_id[0])->second->getStable()->getDMFilesString();
    for (size_t i = 1; i < segments_id.size(); i++)
    {
        if (base_stable != segments.find(segments_id[i])->second->getStable()->getDMFilesString())
            return false;
    }
    return true;
}

PageIdU64 SegmentTestBasic::getRandomSegmentId() // Complexity is O(n)
{
    RUNTIME_CHECK(!segments.empty());
    auto dist = std::uniform_int_distribution<size_t>{0, segments.size() - 1};
    auto pick_n = dist(random);
    auto it = segments.begin();
    std::advance(it, pick_n);
    auto segment_id = it->second->segmentId();
    RUNTIME_CHECK(segments.find(segment_id) != segments.end(), segment_id);
    RUNTIME_CHECK(segments[segment_id]->segmentId() == segment_id);
    return segment_id;
}

size_t SegmentTestBasic::getPageNumAfterGC(StorageType type, NamespaceID ns_id) const
{
    if (storage_pool->uni_ps)
    {
        storage_pool->uni_ps->gc(/* not_skip */ true);
        return storage_pool->uni_ps->getNumberOfPages(UniversalPageIdFormat::toFullPrefix(NullspaceID, type, ns_id));
    }
    else
    {
        assert(storage_pool->log_storage_v3 != nullptr || storage_pool->log_storage_v2 != nullptr);
        switch (type)
        {
        case StorageType::Log:
            if (storage_pool->log_storage_v3)
            {
                storage_pool->log_storage_v3->gc(/* not_skip */ true);
                return storage_pool->log_storage_v3->getNumberOfPages();
            }
            else
            {
                storage_pool->log_storage_v2->gc(/* not_skip */ true);
                return storage_pool->log_storage_v2->getNumberOfPages();
            }
            break;
        case StorageType::Data:
            if (storage_pool->data_storage_v3)
            {
                storage_pool->data_storage_v3->gc(/* not_skip */ true);
                return storage_pool->data_storage_v3->getNumberOfPages();
            }
            else
            {
                storage_pool->data_storage_v2->gc(/* not_skip */ true);
                return storage_pool->data_storage_v2->getNumberOfPages();
            }
            break;
        default:
            throw Exception("", ErrorCodes::NOT_IMPLEMENTED);
        }
    }
}

std::set<PageIdU64> SegmentTestBasic::getAliveExternalPageIdsWithoutGC(NamespaceID ns_id) const
{
    if (storage_pool->uni_ps)
    {
        return *(storage_pool->uni_ps->page_directory->getAliveExternalIds(
            UniversalPageIdFormat::toFullPrefix(NullspaceID, StorageType::Data, ns_id)));
    }
    else
    {
        assert(storage_pool->data_storage_v3 != nullptr || storage_pool->data_storage_v2 != nullptr);
        if (storage_pool->data_storage_v3)
        {
            return storage_pool->data_storage_v3->getAliveExternalPageIds(ns_id);
        }
        else
        {
            return storage_pool->data_storage_v2->getAliveExternalPageIds(ns_id);
        }
    }
}

std::set<PageIdU64> SegmentTestBasic::getAliveExternalPageIdsAfterGC(NamespaceID ns_id) const
{
    if (storage_pool->uni_ps)
    {
        storage_pool->uni_ps->gc(/* not_skip */ true);
        return *(storage_pool->uni_ps->page_directory->getAliveExternalIds(
            UniversalPageIdFormat::toFullPrefix(NullspaceID, StorageType::Data, ns_id)));
    }
    else
    {
        assert(storage_pool->data_storage_v3 != nullptr || storage_pool->data_storage_v2 != nullptr);
        if (storage_pool->data_storage_v3)
        {
            storage_pool->data_storage_v3->gc(/* not_skip */ true);
            return storage_pool->data_storage_v3->getAliveExternalPageIds(ns_id);
        }
        else
        {
            storage_pool->data_storage_v2->gc(/* not_skip */ true);
            return storage_pool->data_storage_v2->getAliveExternalPageIds(ns_id);
        }
    }
}

SegmentPtr SegmentTestBasic::reload(
    bool is_common_handle,
    const ColumnDefinesPtr & pre_define_columns,
    DB::Settings && db_settings)
{
    TiFlashStorageTestBasic::reload(std::move(db_settings));
    storage_path_pool = std::make_shared<StoragePathPool>(db_context->getPathPool().withTable("test", "t1", false));
    storage_pool = std::make_shared<StoragePool>(*db_context, NullspaceID, NAMESPACE_ID, *storage_path_pool, "test.t1");
    storage_pool->restore();
    ColumnDefinesPtr cols = (!pre_define_columns) ? DMTestEnv::getDefaultColumns(
                                is_common_handle ? DMTestEnv::PkType::CommonHandle : DMTestEnv::PkType::HiddenTiDBRowID)
                                                  : pre_define_columns;
    prepareColumns(cols);
    setColumns(cols);

    // Always return the first segment
    return Segment::newSegment(
        Logger::get(),
        *dm_context,
        table_columns,
        RowKeyRange::newAll(is_common_handle, 1),
        DELTA_MERGE_FIRST_SEGMENT_ID,
        0);
}

void SegmentTestBasic::reloadDMContext()
{
    dm_context = createDMContext();
}

std::unique_ptr<DMContext> SegmentTestBasic::createDMContext()
{
    return DMContext::createUnique(
        *db_context,
        storage_path_pool,
        storage_pool,
        /*min_version_*/ 0,
        NullspaceID,
        /*physical_table_id*/ 100,
        /*pk_col_id*/ options.pk_col_id,
        options.is_common_handle,
        1,
        db_context->getSettingsRef());
}

void SegmentTestBasic::setColumns(const ColumnDefinesPtr & columns)
{
    *table_columns = *columns;
    reloadDMContext();
}

void SegmentTestBasic::printFinishedOperations() const
{
    LOG_INFO(logger, "======= Begin Finished Operations Statistics =======");
    LOG_INFO(logger, "Operation Kinds: {}", operation_statistics.size());
    for (auto [name, n] : operation_statistics)
    {
        LOG_INFO(logger, "{}: {}", name, n);
    }
    LOG_INFO(logger, "======= End Finished Operations Statistics =======");
}


Block mergeSegmentRowIds(std::vector<Block> && blocks)
{
    auto accumulated_block = std::move(blocks[0]);
    RUNTIME_CHECK(accumulated_block.segmentRowIdCol() != nullptr);
    for (size_t block_idx = 1; block_idx < blocks.size(); ++block_idx)
    {
        auto block = std::move(blocks[block_idx]);
        auto accu_row_id_col = accumulated_block.segmentRowIdCol();
        auto row_id_col = block.segmentRowIdCol();
        RUNTIME_CHECK(row_id_col != nullptr);
        auto mut_col = (*std::move(accu_row_id_col)).mutate();
        mut_col->insertRangeFrom(*row_id_col, 0, row_id_col->size());
        accumulated_block.setSegmentRowIdCol(std::move(mut_col));
    }
    return accumulated_block;
}

RowKeyRange SegmentTestBasic::buildRowKeyRange(
    Int64 begin,
    Int64 end,
    bool is_common_handle,
    bool including_right_boundary)
{
    // `including_right_boundary` is for creating range like [begin, std::numeric_limits<Int64>::max()) or [begin, std::numeric_limits<Int64>::max()]
    if (including_right_boundary)
        RUNTIME_CHECK(end == std::numeric_limits<Int64>::max());

    if (is_common_handle)
    {
        auto create_rowkey_value = [](Int64 v) {
            WriteBufferFromOwnString ss;
            DB::EncodeUInt(static_cast<UInt8>(TiDB::CodecFlagInt), ss);
            DB::EncodeInt64(v, ss);
            return std::make_shared<String>(ss.releaseStr());
        };
        auto left = RowKeyValue::fromHandle(is_common_handle, create_rowkey_value(begin));
        auto right = including_right_boundary ? RowKeyValue::COMMON_HANDLE_MAX_KEY
                                              : RowKeyValue::fromHandle(is_common_handle, create_rowkey_value(end));
        return RowKeyRange{left, right, is_common_handle, 1};
    }

    auto left = RowKeyValue::fromIntHandle(begin);
    auto right = including_right_boundary ? RowKeyValue::INT_HANDLE_MAX_KEY : RowKeyValue::fromIntHandle(end);
    return RowKeyRange{left, right, is_common_handle, 1};
}

std::pair<SegmentPtr, SegmentSnapshotPtr> SegmentTestBasic::getSegmentForRead(PageIdU64 segment_id)
{
    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    auto snapshot = segment->createSnapshot(
        *dm_context,
        /* for_update */ false,
        CurrentMetrics::DT_SnapshotOfBitmapFilter);
    RUNTIME_CHECK(snapshot != nullptr);
    return {segment, snapshot};
}

std::vector<Block> SegmentTestBasic::readSegment(PageIdU64 segment_id, bool need_row_id, const RowKeyRanges & ranges)
{
    auto [segment, snapshot] = getSegmentForRead(segment_id);
    ColumnDefines columns_to_read = {getExtraHandleColumnDefine(options.is_common_handle), getVersionColumnDefine()};
    auto stream = segment->getInputStreamModeNormal(
        *dm_context,
        columns_to_read,
        snapshot,
        ranges.empty() ? RowKeyRanges{segment->getRowKeyRange()} : ranges,
        {},
        std::numeric_limits<UInt64>::max(),
        DEFAULT_BLOCK_SIZE,
        need_row_id);
    std::vector<Block> blks;
    for (auto blk = stream->read(); blk; blk = stream->read())
    {
        blks.push_back(blk);
    }
    return blks;
}

ColumnPtr SegmentTestBasic::getSegmentRowId(PageIdU64 segment_id, const RowKeyRanges & ranges)
{
    LOG_INFO(logger_op, "getSegmentRowId, segment_id={}", segment_id);
    auto blks = readSegment(segment_id, true, ranges);
    if (blks.empty())
    {
        return nullptr;
    }
    else
    {
        auto block = mergeSegmentRowIds(std::move(blks));
        RUNTIME_CHECK(!block.has(MutSup::extra_handle_column_name));
        RUNTIME_CHECK(block.segmentRowIdCol() != nullptr);
        return block.segmentRowIdCol();
    }
}

ColumnPtr SegmentTestBasic::getSegmentHandle(PageIdU64 segment_id, const RowKeyRanges & ranges)
{
    LOG_INFO(logger_op, "getSegmentHandle, segment_id={}", segment_id);
    auto blks = readSegment(segment_id, false, ranges);
    if (blks.empty())
    {
        return nullptr;
    }
    else
    {
        auto block = vstackBlocks(std::move(blks));
        RUNTIME_CHECK(block.has(MutSup::extra_handle_column_name));
        RUNTIME_CHECK(block.segmentRowIdCol() == nullptr);
        return block.getByName(MutSup::extra_handle_column_name).column;
    }
}

void SegmentTestBasic::writeSegmentWithDeleteRange(
    PageIdU64 segment_id,
    Int64 begin,
    Int64 end,
    bool is_common_handle,
    bool including_right_boundary)
{
    auto range = buildRowKeyRange(begin, end, is_common_handle, including_right_boundary);
    RUNTIME_CHECK(segments.find(segment_id) != segments.end());
    auto segment = segments[segment_id];
    RUNTIME_CHECK(segment->write(*dm_context, range));
}

class SegmentFrameworkTest : public SegmentTestBasic
{
};

TEST_F(SegmentFrameworkTest, PrepareWriteBlock)
try
{
    reloadWithOptions({.is_common_handle = false});

    auto s1_id = splitSegmentAt(DELTA_MERGE_FIRST_SEGMENT_ID, 10);
    ASSERT_TRUE(s1_id.has_value());
    auto s2_id = splitSegmentAt(*s1_id, 20);
    ASSERT_TRUE(s2_id.has_value());

    // s1 has range [10, 20)
    {
        auto [begin, end] = getSegmentKeyRange(*s1_id);
        ASSERT_EQ(10, begin);
        ASSERT_EQ(20, end);
    }

    {
        // write_rows == segment_rows, start_key not specified
        version = 0;
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 10);
        ASSERT_COLUMN_EQ(
            block.getByName(MutSup::extra_handle_column_name),
            createColumn<Int64>({10, 11, 12, 13, 14, 15, 16, 17, 18, 19}));
        ASSERT_COLUMN_EQ(
            block.getByName(MutSup::version_column_name),
            createColumn<UInt64>({1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));
    }
    {
        // write_rows > segment_rows, start_key not specified
        version = 0;
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 13);
        ASSERT_COLUMN_EQ(
            block.getByName(MutSup::extra_handle_column_name),
            createColumn<Int64>({10, 10, 11, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19}));
        ASSERT_COLUMN_EQ(
            block.getByName(MutSup::version_column_name),
            createColumn<UInt64>({1, 2, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1}));
    }
    {
        // start_key specified, end_key - start_key < write_rows
        version = 0;
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 2, /* at */ 16);
        ASSERT_COLUMN_EQ(block.getByName(MutSup::extra_handle_column_name), createColumn<Int64>({16, 17}));
        ASSERT_COLUMN_EQ(block.getByName(MutSup::version_column_name), createColumn<UInt64>({1, 1}));
    }
    {
        version = 0;
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 4, /* at */ 16);
        ASSERT_COLUMN_EQ(block.getByName(MutSup::extra_handle_column_name), createColumn<Int64>({16, 17, 18, 19}));
        ASSERT_COLUMN_EQ(block.getByName(MutSup::version_column_name), createColumn<UInt64>({1, 1, 1, 1}));
    }
    {
        version = 0;
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 5, /* at */ 16);
        ASSERT_COLUMN_EQ(block.getByName(MutSup::extra_handle_column_name), createColumn<Int64>({16, 16, 17, 18, 19}));
        ASSERT_COLUMN_EQ(block.getByName(MutSup::version_column_name), createColumn<UInt64>({1, 2, 1, 1, 1}));
    }
    {
        version = 0;
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 10, /* at */ 16);
        ASSERT_COLUMN_EQ(
            block.getByName(MutSup::extra_handle_column_name),
            createColumn<Int64>({16, 16, 16, 17, 17, 17, 18, 18, 19, 19}));
        ASSERT_COLUMN_EQ(
            block.getByName(MutSup::version_column_name),
            createColumn<UInt64>({1, 2, 3, 1, 2, 3, 1, 2, 1, 2}));
    }
    {
        // write rows < segment rows, start key not specified, should choose a random start.
        auto block = prepareWriteBlockInSegmentRange(*s1_id, 3);
        ASSERT_EQ(3, block.rows());
    }
    {
        // Let's check whether the generated handles will be starting from 12, for at least once.
        auto start_from_12 = 0;
        for (size_t i = 0; i < 100; i++)
        {
            auto block = prepareWriteBlockInSegmentRange(*s1_id, 3);
            if (block.getByName(MutSup::extra_handle_column_name).column->getInt(0) == 12)
                start_from_12++;
        }
        ASSERT_TRUE(start_from_12 > 0); // We should hit at least 1 times in 100 iters.
        ASSERT_TRUE(start_from_12 < 50); // We should not hit 50 times in 100 iters :)
    }
}
CATCH


} // namespace DB::DM::tests
