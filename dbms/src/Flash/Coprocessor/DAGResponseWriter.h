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

#include <Core/Block.h>
#include <Flash/Coprocessor/WaitResult.h>
#include <common/types.h>
#include <tipb/select.pb.h>

namespace DB
{
class DAGContext;

class DAGResponseWriter
{
public:
    DAGResponseWriter(Int64 records_per_chunk_, DAGContext & dag_context_);
    /// prepared with sample block
    virtual void prepare(const Block &){};
    void write(const Block & block)
    {
        if (!doWrite(block))
        {
            notifyNextPipelineWriter();
        }
    }

    // For async writer, `waitForWritable` need to be called before calling `write`.
    // ```
    // auto res = waitForWritable();
    // switch (res) case...
    // write(block);
    // ```
    virtual WaitResult waitForWritable() const { throw Exception("Unsupport"); }

    /// flush cached blocks for batch writer
    void flush()
    {
        if (!doFlush())
        {
            notifyNextPipelineWriter();
        }
    }

    virtual ~DAGResponseWriter() = default;

protected:
    // return true if write is actually write the data
    virtual bool doWrite(const Block & block) = 0;
    // return true if flush is actually flush data
    virtual bool doFlush() = 0;
    virtual void notifyNextPipelineWriter() = 0;

    Int64 records_per_chunk;
    DAGContext & dag_context;
};

} // namespace DB
