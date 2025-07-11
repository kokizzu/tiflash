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

#pragma once

#include <Common/Logger.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Storages/DeltaMerge/ReadMode.h>
#include <Storages/DeltaMerge/ScanContext_fwd.h>
#include <Storages/KVStore/Types.h>
#include <common/types.h>
#include <fmt/format.h>
#include <pingcap/pd/Types.h>
#include <sys/types.h>
#include <tipb/executor.pb.h>

#include <atomic>


namespace DB::DM
{
class PushDownExecutor;
using PushDownExecutorPtr = std::shared_ptr<PushDownExecutor>;
/// ScanContext is used to record statistical information in table scan for current query.
/// For each table scan(one executor id), there is only one ScanContext.
/// ScanContext helps to collect the statistical information of the table scan to show in `EXPLAIN ANALYZE`.
class ScanContext
{
public:
    std::atomic<uint64_t> dmfile_data_scanned_rows{0};
    std::atomic<uint64_t> dmfile_data_skipped_rows{0};
    std::atomic<uint64_t> dmfile_mvcc_scanned_rows{0};
    std::atomic<uint64_t> dmfile_mvcc_skipped_rows{0};
    std::atomic<uint64_t> dmfile_lm_filter_scanned_rows{0};
    std::atomic<uint64_t> dmfile_lm_filter_skipped_rows{0};
    std::atomic<uint64_t> total_dmfile_read_time_ns{0};

    std::atomic<uint64_t> total_rs_pack_filter_check_time_ns{0};
    std::atomic<uint64_t> rs_pack_filter_none{0};
    std::atomic<uint64_t> rs_pack_filter_some{0};
    std::atomic<uint64_t> rs_pack_filter_all{0};
    std::atomic<uint64_t> rs_pack_filter_all_null{0};
    std::atomic<uint64_t> rs_dmfile_read_with_all{0};

    std::atomic<uint64_t> total_remote_region_num{0};
    std::atomic<uint64_t> total_local_region_num{0};
    std::atomic<uint64_t> num_stale_read{0};

    // the read bytes from delta layer and stable layer (in-mem, decompressed)
    std::atomic<uint64_t> user_read_bytes{0};
    std::atomic<uint64_t> disagg_read_cache_hit_size{0};
    std::atomic<uint64_t> disagg_read_cache_miss_size{0};

    // num segments, num tasks
    std::atomic<uint64_t> num_segments{0};
    std::atomic<uint64_t> num_read_tasks{0};
    std::atomic<uint64_t> num_columns{0};

    // delta rows, bytes
    std::atomic<uint64_t> delta_rows{0};
    std::atomic<uint64_t> delta_bytes{0};


    // - read_mode == Normal, apply mvcc to all read blocks
    // - read_mode == Bitmap, it will apply mvcc to get the bitmap
    //   then skip rows according to the mvcc bitmap and push down filter
    //   for other columns
    // - read_mode == Fast, bypass the mvcc
    // mvcc input rows, output rows
    std::atomic<uint64_t> mvcc_input_rows{0};
    std::atomic<uint64_t> mvcc_input_bytes{0};
    std::atomic<uint64_t> mvcc_output_rows{0};

    // Learner read
    std::atomic<uint64_t> learner_read_ns{0};
    // Create snapshot from PageStorage
    std::atomic<uint64_t> create_snapshot_time_ns{0};
    std::atomic<uint64_t> build_inputstream_time_ns{0};
    // Building bitmap
    std::atomic<uint64_t> build_bitmap_time_ns{0};

    std::atomic<uint64_t> vector_idx_load_from_s3{0};
    std::atomic<uint64_t> vector_idx_load_from_disk{0};
    std::atomic<uint64_t> vector_idx_load_from_cache{0};
    std::atomic<uint64_t> vector_idx_load_time_ms{0};
    std::atomic<uint64_t> vector_idx_search_time_ms{0};
    std::atomic<uint64_t> vector_idx_search_visited_nodes{0};
    std::atomic<uint64_t> vector_idx_search_discarded_nodes{0};
    std::atomic<uint64_t> vector_idx_read_vec_time_ms{0};
    std::atomic<uint64_t> vector_idx_read_others_time_ms{0};

    std::atomic<uint32_t> inverted_idx_load_from_s3{0};
    std::atomic<uint32_t> inverted_idx_load_from_disk{0};
    std::atomic<uint32_t> inverted_idx_load_from_cache{0};
    std::atomic<uint64_t> inverted_idx_load_time_ms{0};
    std::atomic<uint64_t> inverted_idx_search_time_ms{0};
    std::atomic<uint32_t> inverted_idx_search_skipped_packs{0};
    std::atomic<uint64_t> inverted_idx_indexed_rows{0};
    std::atomic<uint64_t> inverted_idx_search_selected_rows{0};

    std::atomic<uint32_t> fts_n_from_inmemory_noindex{0};
    std::atomic<uint32_t> fts_n_from_tiny_index{0};
    std::atomic<uint32_t> fts_n_from_tiny_noindex{0};
    std::atomic<uint32_t> fts_n_from_dmf_index{0};
    std::atomic<uint32_t> fts_n_from_dmf_noindex{0};
    std::atomic<uint64_t> fts_rows_from_inmemory_noindex{0};
    std::atomic<uint64_t> fts_rows_from_tiny_index{0};
    std::atomic<uint64_t> fts_rows_from_tiny_noindex{0};
    std::atomic<uint64_t> fts_rows_from_dmf_index{0};
    std::atomic<uint64_t> fts_rows_from_dmf_noindex{0};
    std::atomic<uint64_t> fts_idx_load_total_ms{0};
    std::atomic<uint32_t> fts_idx_load_from_cache{0};
    std::atomic<uint32_t> fts_idx_load_from_column_file{0};
    std::atomic<uint32_t> fts_idx_load_from_stable_s3{0};
    std::atomic<uint32_t> fts_idx_load_from_stable_disk{0};
    std::atomic<uint32_t> fts_idx_search_n{0};
    std::atomic<uint64_t> fts_idx_search_total_ms{0};
    std::atomic<uint64_t> fts_idx_dm_search_rows{0};
    std::atomic<uint64_t> fts_idx_dm_total_read_fts_ms{0};
    std::atomic<uint64_t> fts_idx_dm_total_read_others_ms{0};
    std::atomic<uint64_t> fts_idx_tiny_search_rows{0};
    std::atomic<uint64_t> fts_idx_tiny_total_read_fts_ms{0};
    std::atomic<uint64_t> fts_idx_tiny_total_read_others_ms{0};
    std::atomic<uint64_t> fts_brute_total_read_ms{0};
    std::atomic<uint64_t> fts_brute_total_search_ms{0};

    const KeyspaceID keyspace_id;
    ReadMode read_mode = ReadMode::Normal; // note: share struct padding with keyspace_id
    const String resource_group_name;
    PushDownExecutorPtr pushdown_executor;

    explicit ScanContext(const KeyspaceID & keyspace_id_ = NullspaceID, const String & name = "")
        : keyspace_id(keyspace_id_)
        , resource_group_name(name)
    {}

    void deserialize(const tipb::TiFlashScanContext & tiflash_scan_context_pb)
    {
        dmfile_data_scanned_rows = tiflash_scan_context_pb.dmfile_data_scanned_rows();
        dmfile_data_skipped_rows = tiflash_scan_context_pb.dmfile_data_skipped_rows();
        dmfile_mvcc_scanned_rows = tiflash_scan_context_pb.dmfile_mvcc_scanned_rows();
        dmfile_mvcc_skipped_rows = tiflash_scan_context_pb.dmfile_mvcc_skipped_rows();
        dmfile_lm_filter_scanned_rows = tiflash_scan_context_pb.dmfile_lm_filter_scanned_rows();
        dmfile_lm_filter_skipped_rows = tiflash_scan_context_pb.dmfile_lm_filter_skipped_rows();
        total_rs_pack_filter_check_time_ns = tiflash_scan_context_pb.total_dmfile_rs_check_ms() * 1000000;
        // TODO: rs_pack_filter_none, rs_pack_filter_some, rs_pack_filter_all,rs_pack_filter_all_null
        // rs_dmfile_read_with_all
        total_dmfile_read_time_ns = tiflash_scan_context_pb.total_dmfile_read_ms() * 1000000;
        create_snapshot_time_ns = tiflash_scan_context_pb.total_build_snapshot_ms() * 1000000;
        total_remote_region_num = tiflash_scan_context_pb.remote_regions();
        total_local_region_num = tiflash_scan_context_pb.local_regions();
        user_read_bytes = tiflash_scan_context_pb.user_read_bytes();
        learner_read_ns = tiflash_scan_context_pb.total_learner_read_ms() * 1000000;
        disagg_read_cache_hit_size = tiflash_scan_context_pb.disagg_read_cache_hit_bytes();
        disagg_read_cache_miss_size = tiflash_scan_context_pb.disagg_read_cache_miss_bytes();

        num_segments = tiflash_scan_context_pb.segments();
        num_read_tasks = tiflash_scan_context_pb.read_tasks();

        delta_rows = tiflash_scan_context_pb.delta_rows();
        delta_bytes = tiflash_scan_context_pb.delta_bytes();

        mvcc_input_rows = tiflash_scan_context_pb.mvcc_input_rows();
        mvcc_input_bytes = tiflash_scan_context_pb.mvcc_input_bytes();
        mvcc_output_rows = tiflash_scan_context_pb.mvcc_output_rows();
        build_bitmap_time_ns = tiflash_scan_context_pb.total_build_bitmap_ms() * 1000000;
        num_stale_read = tiflash_scan_context_pb.stale_read_regions();
        build_inputstream_time_ns = tiflash_scan_context_pb.total_build_inputstream_ms() * 1000000;

        setStreamCost(
            tiflash_scan_context_pb.min_local_stream_ms() * 1000000,
            tiflash_scan_context_pb.max_local_stream_ms() * 1000000,
            tiflash_scan_context_pb.min_remote_stream_ms() * 1000000,
            tiflash_scan_context_pb.max_remote_stream_ms() * 1000000);

        deserializeRegionNumberOfInstance(tiflash_scan_context_pb);

        vector_idx_load_from_s3 = tiflash_scan_context_pb.vector_idx_load_from_s3();
        vector_idx_load_from_disk = tiflash_scan_context_pb.vector_idx_load_from_disk();
        vector_idx_load_from_cache = tiflash_scan_context_pb.vector_idx_load_from_cache();
        vector_idx_load_time_ms = tiflash_scan_context_pb.vector_idx_load_time_ms();
        vector_idx_search_time_ms = tiflash_scan_context_pb.vector_idx_search_time_ms();
        vector_idx_search_visited_nodes = tiflash_scan_context_pb.vector_idx_search_visited_nodes();
        vector_idx_search_discarded_nodes = tiflash_scan_context_pb.vector_idx_search_discarded_nodes();
        vector_idx_read_vec_time_ms = tiflash_scan_context_pb.vector_idx_read_vec_time_ms();
        vector_idx_read_others_time_ms = tiflash_scan_context_pb.vector_idx_read_others_time_ms();

        inverted_idx_load_from_s3 = tiflash_scan_context_pb.inverted_idx_load_from_s3();
        inverted_idx_load_from_disk = tiflash_scan_context_pb.inverted_idx_load_from_disk();
        inverted_idx_load_from_cache = tiflash_scan_context_pb.inverted_idx_load_from_cache();
        inverted_idx_load_time_ms = tiflash_scan_context_pb.inverted_idx_load_time_ms();
        inverted_idx_search_time_ms = tiflash_scan_context_pb.inverted_idx_search_time_ms();
        inverted_idx_search_skipped_packs = tiflash_scan_context_pb.inverted_idx_search_skipped_packs();
        inverted_idx_indexed_rows = tiflash_scan_context_pb.inverted_idx_indexed_rows();
        inverted_idx_search_selected_rows = tiflash_scan_context_pb.inverted_idx_search_selected_rows();

        fts_n_from_inmemory_noindex = tiflash_scan_context_pb.fts_n_from_inmemory_noindex();
        fts_n_from_tiny_index = tiflash_scan_context_pb.fts_n_from_tiny_index();
        fts_n_from_tiny_noindex = tiflash_scan_context_pb.fts_n_from_tiny_noindex();
        fts_n_from_dmf_index = tiflash_scan_context_pb.fts_n_from_dmf_index();
        fts_n_from_dmf_noindex = tiflash_scan_context_pb.fts_n_from_dmf_noindex();
        fts_rows_from_inmemory_noindex = tiflash_scan_context_pb.fts_rows_from_inmemory_noindex();
        fts_rows_from_tiny_index = tiflash_scan_context_pb.fts_rows_from_tiny_index();
        fts_rows_from_tiny_noindex = tiflash_scan_context_pb.fts_rows_from_tiny_noindex();
        fts_rows_from_dmf_index = tiflash_scan_context_pb.fts_rows_from_dmf_index();
        fts_rows_from_dmf_noindex = tiflash_scan_context_pb.fts_rows_from_dmf_noindex();
        fts_idx_load_total_ms = tiflash_scan_context_pb.fts_idx_load_total_ms();
        fts_idx_load_from_cache = tiflash_scan_context_pb.fts_idx_load_from_cache();
        fts_idx_load_from_column_file = tiflash_scan_context_pb.fts_idx_load_from_column_file();
        fts_idx_load_from_stable_s3 = tiflash_scan_context_pb.fts_idx_load_from_stable_s3();
        fts_idx_load_from_stable_disk = tiflash_scan_context_pb.fts_idx_load_from_stable_disk();
        fts_idx_search_n = tiflash_scan_context_pb.fts_idx_search_n();
        fts_idx_search_total_ms = tiflash_scan_context_pb.fts_idx_search_total_ms();
        fts_idx_dm_search_rows = tiflash_scan_context_pb.fts_idx_dm_search_rows();
        fts_idx_dm_total_read_fts_ms = tiflash_scan_context_pb.fts_idx_dm_total_read_fts_ms();
        fts_idx_dm_total_read_others_ms = tiflash_scan_context_pb.fts_idx_dm_total_read_others_ms();
        fts_idx_tiny_search_rows = tiflash_scan_context_pb.fts_idx_tiny_search_rows();
        fts_idx_tiny_total_read_fts_ms = tiflash_scan_context_pb.fts_idx_tiny_total_read_fts_ms();
        fts_idx_tiny_total_read_others_ms = tiflash_scan_context_pb.fts_idx_tiny_total_read_others_ms();
        fts_brute_total_read_ms = tiflash_scan_context_pb.fts_brute_total_read_ms();
        fts_brute_total_search_ms = tiflash_scan_context_pb.fts_brute_total_search_ms();
    }

    tipb::TiFlashScanContext serialize()
    {
        tipb::TiFlashScanContext tiflash_scan_context_pb{};
        tiflash_scan_context_pb.set_dmfile_data_scanned_rows(dmfile_data_scanned_rows);
        tiflash_scan_context_pb.set_dmfile_data_skipped_rows(dmfile_data_skipped_rows);
        tiflash_scan_context_pb.set_dmfile_mvcc_scanned_rows(dmfile_mvcc_scanned_rows);
        tiflash_scan_context_pb.set_dmfile_mvcc_skipped_rows(dmfile_mvcc_skipped_rows);
        tiflash_scan_context_pb.set_dmfile_lm_filter_scanned_rows(dmfile_lm_filter_scanned_rows);
        tiflash_scan_context_pb.set_dmfile_lm_filter_skipped_rows(dmfile_lm_filter_skipped_rows);
        tiflash_scan_context_pb.set_total_dmfile_rs_check_ms(total_rs_pack_filter_check_time_ns / 1000000);
        // TODO: pack_filter_none, pack_filter_some, pack_filter_all
        tiflash_scan_context_pb.set_total_dmfile_read_ms(total_dmfile_read_time_ns / 1000000);
        tiflash_scan_context_pb.set_total_build_snapshot_ms(create_snapshot_time_ns / 1000000);
        tiflash_scan_context_pb.set_remote_regions(total_remote_region_num);
        tiflash_scan_context_pb.set_local_regions(total_local_region_num);
        tiflash_scan_context_pb.set_user_read_bytes(user_read_bytes);
        tiflash_scan_context_pb.set_total_learner_read_ms(learner_read_ns / 1000000);
        tiflash_scan_context_pb.set_disagg_read_cache_hit_bytes(disagg_read_cache_hit_size);
        tiflash_scan_context_pb.set_disagg_read_cache_miss_bytes(disagg_read_cache_miss_size);

        tiflash_scan_context_pb.set_segments(num_segments);
        tiflash_scan_context_pb.set_read_tasks(num_read_tasks);

        tiflash_scan_context_pb.set_delta_rows(delta_rows);
        tiflash_scan_context_pb.set_delta_bytes(delta_bytes);

        tiflash_scan_context_pb.set_mvcc_input_rows(mvcc_input_rows);
        tiflash_scan_context_pb.set_mvcc_input_bytes(mvcc_input_bytes);
        tiflash_scan_context_pb.set_mvcc_output_rows(mvcc_output_rows);
        tiflash_scan_context_pb.set_total_build_bitmap_ms(build_bitmap_time_ns / 1000000);
        tiflash_scan_context_pb.set_stale_read_regions(num_stale_read);
        tiflash_scan_context_pb.set_total_build_inputstream_ms(build_inputstream_time_ns / 1000000);

        tiflash_scan_context_pb.set_min_local_stream_ms(local_min_stream_cost_ns / 1000000);
        tiflash_scan_context_pb.set_max_local_stream_ms(local_max_stream_cost_ns / 1000000);
        tiflash_scan_context_pb.set_min_remote_stream_ms(remote_min_stream_cost_ns / 1000000);
        tiflash_scan_context_pb.set_max_remote_stream_ms(remote_max_stream_cost_ns / 1000000);

        serializeRegionNumOfInstance(tiflash_scan_context_pb);

        tiflash_scan_context_pb.set_vector_idx_load_from_s3(vector_idx_load_from_s3);
        tiflash_scan_context_pb.set_vector_idx_load_from_disk(vector_idx_load_from_disk);
        tiflash_scan_context_pb.set_vector_idx_load_from_cache(vector_idx_load_from_cache);
        tiflash_scan_context_pb.set_vector_idx_load_time_ms(vector_idx_load_time_ms);
        tiflash_scan_context_pb.set_vector_idx_search_time_ms(vector_idx_search_time_ms);
        tiflash_scan_context_pb.set_vector_idx_search_visited_nodes(vector_idx_search_visited_nodes);
        tiflash_scan_context_pb.set_vector_idx_search_discarded_nodes(vector_idx_search_discarded_nodes);
        tiflash_scan_context_pb.set_vector_idx_read_vec_time_ms(vector_idx_read_vec_time_ms);
        tiflash_scan_context_pb.set_vector_idx_read_others_time_ms(vector_idx_read_others_time_ms);

        tiflash_scan_context_pb.set_inverted_idx_load_from_s3(inverted_idx_load_from_s3);
        tiflash_scan_context_pb.set_inverted_idx_load_from_disk(inverted_idx_load_from_disk);
        tiflash_scan_context_pb.set_inverted_idx_load_from_cache(inverted_idx_load_from_cache);
        tiflash_scan_context_pb.set_inverted_idx_load_time_ms(inverted_idx_load_time_ms);
        tiflash_scan_context_pb.set_inverted_idx_search_time_ms(inverted_idx_search_time_ms);
        tiflash_scan_context_pb.set_inverted_idx_search_skipped_packs(inverted_idx_search_skipped_packs);
        tiflash_scan_context_pb.set_inverted_idx_indexed_rows(inverted_idx_indexed_rows);
        tiflash_scan_context_pb.set_inverted_idx_search_selected_rows(inverted_idx_search_selected_rows);

        tiflash_scan_context_pb.set_fts_n_from_inmemory_noindex(fts_n_from_inmemory_noindex);
        tiflash_scan_context_pb.set_fts_n_from_tiny_index(fts_n_from_tiny_index);
        tiflash_scan_context_pb.set_fts_n_from_tiny_noindex(fts_n_from_tiny_noindex);
        tiflash_scan_context_pb.set_fts_n_from_dmf_index(fts_n_from_dmf_index);
        tiflash_scan_context_pb.set_fts_n_from_dmf_noindex(fts_n_from_dmf_noindex);
        tiflash_scan_context_pb.set_fts_rows_from_inmemory_noindex(fts_rows_from_inmemory_noindex);
        tiflash_scan_context_pb.set_fts_rows_from_tiny_index(fts_rows_from_tiny_index);
        tiflash_scan_context_pb.set_fts_rows_from_tiny_noindex(fts_rows_from_tiny_noindex);
        tiflash_scan_context_pb.set_fts_rows_from_dmf_index(fts_rows_from_dmf_index);
        tiflash_scan_context_pb.set_fts_rows_from_dmf_noindex(fts_rows_from_dmf_noindex);
        tiflash_scan_context_pb.set_fts_idx_load_total_ms(fts_idx_load_total_ms);
        tiflash_scan_context_pb.set_fts_idx_load_from_cache(fts_idx_load_from_cache);
        tiflash_scan_context_pb.set_fts_idx_load_from_column_file(fts_idx_load_from_column_file);
        tiflash_scan_context_pb.set_fts_idx_load_from_stable_s3(fts_idx_load_from_stable_s3);
        tiflash_scan_context_pb.set_fts_idx_load_from_stable_disk(fts_idx_load_from_stable_disk);
        tiflash_scan_context_pb.set_fts_idx_search_n(fts_idx_search_n);
        tiflash_scan_context_pb.set_fts_idx_search_total_ms(fts_idx_search_total_ms);
        tiflash_scan_context_pb.set_fts_idx_dm_search_rows(fts_idx_dm_search_rows);
        tiflash_scan_context_pb.set_fts_idx_dm_total_read_fts_ms(fts_idx_dm_total_read_fts_ms);
        tiflash_scan_context_pb.set_fts_idx_dm_total_read_others_ms(fts_idx_dm_total_read_others_ms);
        tiflash_scan_context_pb.set_fts_idx_tiny_search_rows(fts_idx_tiny_search_rows);
        tiflash_scan_context_pb.set_fts_idx_tiny_total_read_fts_ms(fts_idx_tiny_total_read_fts_ms);
        tiflash_scan_context_pb.set_fts_idx_tiny_total_read_others_ms(fts_idx_tiny_total_read_others_ms);
        tiflash_scan_context_pb.set_fts_brute_total_read_ms(fts_brute_total_read_ms);
        tiflash_scan_context_pb.set_fts_brute_total_search_ms(fts_brute_total_search_ms);

        return tiflash_scan_context_pb;
    }

    void merge(const ScanContext & other)
    {
        dmfile_data_scanned_rows += other.dmfile_data_scanned_rows;
        dmfile_data_skipped_rows += other.dmfile_data_skipped_rows;
        dmfile_mvcc_scanned_rows += other.dmfile_mvcc_scanned_rows;
        dmfile_mvcc_skipped_rows += other.dmfile_mvcc_skipped_rows;
        dmfile_lm_filter_scanned_rows += other.dmfile_lm_filter_scanned_rows;
        dmfile_lm_filter_skipped_rows += other.dmfile_lm_filter_skipped_rows;
        total_rs_pack_filter_check_time_ns += other.total_rs_pack_filter_check_time_ns;
        rs_pack_filter_none += other.rs_pack_filter_none;
        rs_pack_filter_some += other.rs_pack_filter_some;
        rs_pack_filter_all += other.rs_pack_filter_all;
        rs_pack_filter_all_null += other.rs_pack_filter_all_null;
        rs_dmfile_read_with_all += other.rs_dmfile_read_with_all;
        total_dmfile_read_time_ns += other.total_dmfile_read_time_ns;

        total_local_region_num += other.total_local_region_num;
        total_remote_region_num += other.total_remote_region_num;
        user_read_bytes += other.user_read_bytes;
        disagg_read_cache_hit_size += other.disagg_read_cache_hit_size;
        disagg_read_cache_miss_size += other.disagg_read_cache_miss_size;

        num_segments += other.num_segments;
        num_read_tasks += other.num_read_tasks;
        // num_columns should not sum

        delta_rows += other.delta_rows;
        delta_bytes += other.delta_bytes;

        mvcc_input_rows += other.mvcc_input_rows;
        mvcc_input_bytes += other.mvcc_input_bytes;
        mvcc_output_rows += other.mvcc_output_rows;

        learner_read_ns += other.learner_read_ns;
        create_snapshot_time_ns += other.create_snapshot_time_ns;
        build_inputstream_time_ns += other.build_inputstream_time_ns;
        build_bitmap_time_ns += other.build_bitmap_time_ns;

        num_stale_read += other.num_stale_read;

        mergeStreamCost(
            other.local_min_stream_cost_ns,
            other.local_max_stream_cost_ns,
            other.remote_min_stream_cost_ns,
            other.remote_max_stream_cost_ns);

        mergeRegionNumberOfInstance(other);

        vector_idx_load_from_s3 += other.vector_idx_load_from_s3;
        vector_idx_load_from_disk += other.vector_idx_load_from_disk;
        vector_idx_load_from_cache += other.vector_idx_load_from_cache;
        vector_idx_load_time_ms += other.vector_idx_load_time_ms;
        vector_idx_search_time_ms += other.vector_idx_search_time_ms;
        vector_idx_search_visited_nodes += other.vector_idx_search_visited_nodes;
        vector_idx_search_discarded_nodes += other.vector_idx_search_discarded_nodes;
        vector_idx_read_vec_time_ms += other.vector_idx_read_vec_time_ms;
        vector_idx_read_others_time_ms += other.vector_idx_read_others_time_ms;

        inverted_idx_load_from_s3 += other.inverted_idx_load_from_s3;
        inverted_idx_load_from_disk += other.inverted_idx_load_from_disk;
        inverted_idx_load_from_cache += other.inverted_idx_load_from_cache;
        inverted_idx_load_time_ms += other.inverted_idx_load_time_ms;
        inverted_idx_search_time_ms += other.inverted_idx_search_time_ms;
        inverted_idx_search_skipped_packs += other.inverted_idx_search_skipped_packs;
        inverted_idx_indexed_rows += other.inverted_idx_indexed_rows;
        inverted_idx_search_selected_rows += other.inverted_idx_search_selected_rows;

        fts_n_from_inmemory_noindex += other.fts_n_from_inmemory_noindex;
        fts_n_from_tiny_index += other.fts_n_from_tiny_index;
        fts_n_from_tiny_noindex += other.fts_n_from_tiny_noindex;
        fts_n_from_dmf_index += other.fts_n_from_dmf_index;
        fts_n_from_dmf_noindex += other.fts_n_from_dmf_noindex;
        fts_rows_from_inmemory_noindex += other.fts_rows_from_inmemory_noindex;
        fts_rows_from_tiny_index += other.fts_rows_from_tiny_index;
        fts_rows_from_tiny_noindex += other.fts_rows_from_tiny_noindex;
        fts_rows_from_dmf_index += other.fts_rows_from_dmf_index;
        fts_rows_from_dmf_noindex += other.fts_rows_from_dmf_noindex;
        fts_idx_load_total_ms += other.fts_idx_load_total_ms;
        fts_idx_load_from_cache += other.fts_idx_load_from_cache;
        fts_idx_load_from_column_file += other.fts_idx_load_from_column_file;
        fts_idx_load_from_stable_s3 += other.fts_idx_load_from_stable_s3;
        fts_idx_load_from_stable_disk += other.fts_idx_load_from_stable_disk;
        fts_idx_search_n += other.fts_idx_search_n;
        fts_idx_search_total_ms += other.fts_idx_search_total_ms;
        fts_idx_dm_search_rows += other.fts_idx_dm_search_rows;
        fts_idx_dm_total_read_fts_ms += other.fts_idx_dm_total_read_fts_ms;
        fts_idx_dm_total_read_others_ms += other.fts_idx_dm_total_read_others_ms;
        fts_idx_tiny_search_rows += other.fts_idx_tiny_search_rows;
        fts_idx_tiny_total_read_fts_ms += other.fts_idx_tiny_total_read_fts_ms;
        fts_idx_tiny_total_read_others_ms += other.fts_idx_tiny_total_read_others_ms;
        fts_brute_total_read_ms += other.fts_brute_total_read_ms;
        fts_brute_total_search_ms += other.fts_brute_total_search_ms;
    }

    void merge(const tipb::TiFlashScanContext & other)
    {
        dmfile_data_scanned_rows += other.dmfile_data_scanned_rows();
        dmfile_data_skipped_rows += other.dmfile_data_skipped_rows();
        dmfile_mvcc_scanned_rows += other.dmfile_mvcc_scanned_rows();
        dmfile_mvcc_skipped_rows += other.dmfile_mvcc_skipped_rows();
        dmfile_lm_filter_scanned_rows += other.dmfile_lm_filter_scanned_rows();
        dmfile_lm_filter_skipped_rows += other.dmfile_lm_filter_skipped_rows();
        total_rs_pack_filter_check_time_ns += other.total_dmfile_rs_check_ms() * 1000000;
        // TODO: rs_pack_filter_none, rs_pack_filter_some, rs_pack_filter_all, rs_pack_filter_all_null
        // rs_dmfile_read_with_all
        total_dmfile_read_time_ns += other.total_dmfile_read_ms() * 1000000;
        create_snapshot_time_ns += other.total_build_snapshot_ms() * 1000000;
        total_local_region_num += other.local_regions();
        total_remote_region_num += other.remote_regions();
        user_read_bytes += other.user_read_bytes();
        learner_read_ns += other.total_learner_read_ms() * 1000000;
        disagg_read_cache_hit_size += other.disagg_read_cache_hit_bytes();
        disagg_read_cache_miss_size += other.disagg_read_cache_miss_bytes();

        num_segments += other.segments();
        num_read_tasks += other.read_tasks();

        delta_rows += other.delta_rows();
        delta_bytes += other.delta_bytes();

        mvcc_input_rows += other.mvcc_input_rows();
        mvcc_input_bytes += other.mvcc_input_bytes();
        mvcc_output_rows += other.mvcc_output_rows();
        build_bitmap_time_ns += other.total_build_bitmap_ms() * 1000000;
        num_stale_read += other.stale_read_regions();
        build_inputstream_time_ns += other.total_build_inputstream_ms() * 1000000;

        mergeStreamCost(
            other.min_local_stream_ms() * 1000000,
            other.max_local_stream_ms() * 1000000,
            other.min_remote_stream_ms() * 1000000,
            other.max_remote_stream_ms() * 1000000);

        mergeRegionNumberOfInstance(other);

        vector_idx_load_from_s3 += other.vector_idx_load_from_s3();
        vector_idx_load_from_disk += other.vector_idx_load_from_disk();
        vector_idx_load_from_cache += other.vector_idx_load_from_cache();
        vector_idx_load_time_ms += other.vector_idx_load_time_ms();
        vector_idx_search_time_ms += other.vector_idx_search_time_ms();
        vector_idx_search_visited_nodes += other.vector_idx_search_visited_nodes();
        vector_idx_search_discarded_nodes += other.vector_idx_search_discarded_nodes();
        vector_idx_read_vec_time_ms += other.vector_idx_read_vec_time_ms();
        vector_idx_read_others_time_ms += other.vector_idx_read_others_time_ms();

        inverted_idx_load_from_s3 += other.inverted_idx_load_from_s3();
        inverted_idx_load_from_disk += other.inverted_idx_load_from_disk();
        inverted_idx_load_from_cache += other.inverted_idx_load_from_cache();
        inverted_idx_load_time_ms += other.inverted_idx_load_time_ms();
        inverted_idx_search_time_ms += other.inverted_idx_search_time_ms();
        inverted_idx_search_skipped_packs += other.inverted_idx_search_skipped_packs();
        inverted_idx_indexed_rows += other.inverted_idx_indexed_rows();
        inverted_idx_search_selected_rows += other.inverted_idx_search_selected_rows();

        fts_n_from_inmemory_noindex += other.fts_n_from_inmemory_noindex();
        fts_n_from_tiny_index += other.fts_n_from_tiny_index();
        fts_n_from_tiny_noindex += other.fts_n_from_tiny_noindex();
        fts_n_from_dmf_index += other.fts_n_from_dmf_index();
        fts_n_from_dmf_noindex += other.fts_n_from_dmf_noindex();
        fts_rows_from_inmemory_noindex += other.fts_rows_from_inmemory_noindex();
        fts_rows_from_tiny_index += other.fts_rows_from_tiny_index();
        fts_rows_from_tiny_noindex += other.fts_rows_from_tiny_noindex();
        fts_rows_from_dmf_index += other.fts_rows_from_dmf_index();
        fts_rows_from_dmf_noindex += other.fts_rows_from_dmf_noindex();
        fts_idx_load_total_ms += other.fts_idx_load_total_ms();
        fts_idx_load_from_cache += other.fts_idx_load_from_cache();
        fts_idx_load_from_column_file += other.fts_idx_load_from_column_file();
        fts_idx_load_from_stable_s3 += other.fts_idx_load_from_stable_s3();
        fts_idx_load_from_stable_disk += other.fts_idx_load_from_stable_disk();
        fts_idx_search_n += other.fts_idx_search_n();
        fts_idx_search_total_ms += other.fts_idx_search_total_ms();
        fts_idx_dm_search_rows += other.fts_idx_dm_search_rows();
        fts_idx_dm_total_read_fts_ms += other.fts_idx_dm_total_read_fts_ms();
        fts_idx_dm_total_read_others_ms += other.fts_idx_dm_total_read_others_ms();
        fts_idx_tiny_search_rows += other.fts_idx_tiny_search_rows();
        fts_idx_tiny_total_read_fts_ms += other.fts_idx_tiny_total_read_fts_ms();
        fts_idx_tiny_total_read_others_ms += other.fts_idx_tiny_total_read_others_ms();
        fts_brute_total_read_ms += other.fts_brute_total_read_ms();
        fts_brute_total_search_ms += other.fts_brute_total_search_ms();
    }

    String toJson() const;

    void setRegionNumOfCurrentInstance(uint64_t region_num);
    void setStreamCost(uint64_t local_min_ns, uint64_t local_max_ns, uint64_t remote_min_ns, uint64_t remote_max_ns);

    static void initCurrentInstanceId(Poco::Util::AbstractConfiguration & config, const LoggerPtr & log);

private:
    void serializeRegionNumOfInstance(tipb::TiFlashScanContext & proto) const;
    void deserializeRegionNumberOfInstance(const tipb::TiFlashScanContext & proto);
    void mergeRegionNumberOfInstance(const ScanContext & other);
    void mergeRegionNumberOfInstance(const tipb::TiFlashScanContext & other);
    void mergeStreamCost(uint64_t local_min_ns, uint64_t local_max_ns, uint64_t remote_min_ns, uint64_t remote_max_ns);

    // instance_id -> number of regions.
    // `region_num_of_instance` is accessed by a single thread.
    using RegionNumOfInstance = std::unordered_map<String, uint64_t>;
    RegionNumOfInstance region_num_of_instance;

    // These members `*_stream_cost_ns` are accessed by a single thread.
    uint64_t local_min_stream_cost_ns{0};
    uint64_t local_max_stream_cost_ns{0};
    uint64_t remote_min_stream_cost_ns{0};
    uint64_t remote_max_stream_cost_ns{0};

    // `current_instance_id` is a identification of this store.
    // It only used to identify which store generated the ScanContext object.
    inline static String current_instance_id;
};

} // namespace DB::DM
