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

#include <Storages/DeltaMerge/DeltaMergeDefines.h>
#include <Storages/DeltaMerge/File/DMFile_fwd.h>
#include <Storages/DeltaMerge/Index/LocalIndexCache_fwd.h>
#include <Storages/DeltaMerge/Index/VectorIndex/Reader.h>
#include <Storages/DeltaMerge/ScanContext_fwd.h>

namespace DB::DM
{

class DMFileVectorIndexReader
{
private:
    const DMFilePtr & dmfile;
    const ANNQueryInfoPtr & ann_query_info;
    const BitmapFilterView valid_rows;
    const ScanContextPtr & scan_context;
    // Global local index cache
    const LocalIndexCachePtr local_index_cache;

    // Performance statistics
    struct PerfStat
    {
        double duration_search;
        double duration_load_index;
        double duration_read_vec_column;
        size_t index_size;
        size_t visited_nodes;
        size_t discarded_nodes;
        size_t selected_nodes;
        bool has_s3_download;
        bool has_load_from_file;

        String toString() const;
    };
    PerfStat perf_stat;

    // Set after load().
    VectorIndexReaderPtr vec_index = nullptr;
    bool loaded = false;

public:
    DMFileVectorIndexReader(
        const ANNQueryInfoPtr & ann_query_info_,
        const DMFilePtr & dmfile_,
        const BitmapFilterView & valid_rows_,
        const ScanContextPtr & scan_context_,
        const LocalIndexCachePtr & local_index_cache_)
        : dmfile(dmfile_)
        , ann_query_info(ann_query_info_)
        , valid_rows(valid_rows_)
        , scan_context(scan_context_)
        , local_index_cache(local_index_cache_)
        , perf_stat()
    {}

    ~DMFileVectorIndexReader();

    // Read vector column data with the specified rowids.
    void read(MutableColumnPtr & vec_column, const std::span<const VectorIndexReader::Key> & selected_rows);

    // Load vector index and search results.
    // Return the rowids of the selected rows.
    std::vector<VectorIndexReader::SearchResult> load();

    String perfStat() const;

private:
    void loadVectorIndex();
    std::vector<VectorIndexReader::SearchResult> loadVectorSearchResult();
};

using DMFileVectorIndexReaderPtr = std::shared_ptr<DMFileVectorIndexReader>;

} // namespace DB::DM
