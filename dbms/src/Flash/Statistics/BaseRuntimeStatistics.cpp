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

#include <Common/TiFlashException.h>
#include <DataStreams/BlockStreamProfileInfo.h>
#include <Flash/Statistics/BaseRuntimeStatistics.h>
#include <Flash/Statistics/ConnectionProfileInfo.h>
#include <Operators/OperatorProfileInfo.h>

namespace DB
{
void BaseRuntimeStatistics::append(const BlockStreamProfileInfo & profile_info)
{
    rows += profile_info.rows;
    blocks += profile_info.blocks;
    bytes += profile_info.bytes;
    allocated_bytes += profile_info.allocated_bytes;
    execution_time_ns = std::max(execution_time_ns, profile_info.execution_time);
    ++concurrency;
}

void BaseRuntimeStatistics::append(const OperatorProfileInfo & profile_info)
{
    rows += profile_info.rows;
    blocks += profile_info.blocks;
    bytes += profile_info.bytes;
    allocated_bytes += profile_info.allocated_bytes;
    if (execution_time_ns < profile_info.execution_time)
    {
        execution_time_ns = profile_info.execution_time;
        queue_wait_time_ns = profile_info.task_wait_time;
        pipeline_breaker_wait_time_ns = profile_info.pipeline_breaker_wait_time;
    }
    ++concurrency;
}

void BaseRuntimeStatistics::updateReceiveConnectionInfo(const ConnectionProfileInfo & conn_profile_info)
{
    switch (conn_profile_info.type)
    {
    case DB::ConnectionProfileInfo::Local:
        break;
    case ConnectionProfileInfo::InnerZoneRemote:
        inner_zone_receive_bytes += conn_profile_info.bytes;
        break;
    case DB::ConnectionProfileInfo::InterZoneRemote:
        inter_zone_receive_bytes += conn_profile_info.bytes;
        break;
    default:
        throw TiFlashException(
            fmt::format("{} connection profile type is unexpected", fmt::underlying(conn_profile_info.type)),
            Errors::Planner::Internal);
    }
}

void BaseRuntimeStatistics::updateSendConnectionInfo(const ConnectionProfileInfo & conn_profile_info)
{
    switch (conn_profile_info.type)
    {
    case DB::ConnectionProfileInfo::Local:
        break;
    case ConnectionProfileInfo::InnerZoneRemote:
        inner_zone_send_bytes += conn_profile_info.bytes;
        break;
    case DB::ConnectionProfileInfo::InterZoneRemote:
        inter_zone_send_bytes += conn_profile_info.bytes;
        break;
    default:
        throw TiFlashException(
            fmt::format("{} connection profile type is unexpected", fmt::underlying(conn_profile_info.type)),
            Errors::Planner::Internal);
    }
}
} // namespace DB
