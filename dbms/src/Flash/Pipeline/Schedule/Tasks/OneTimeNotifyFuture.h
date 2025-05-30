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

#include <Flash/Pipeline/Schedule/Tasks/NotifyFuture.h>
#include <Flash/Pipeline/Schedule/Tasks/PipeConditionVariable.h>

#include <mutex>

namespace DB
{
/// `Finish` will notify all tasks registered in the `one-time-future` and tasks registered after "finish" are notified immediately.
class OneTimeNotifyFuture : public NotifyFuture
{
public:
    explicit OneTimeNotifyFuture(NotifyType notify_type_)
        : notify_type(notify_type_)
    {}
    void registerTask(TaskPtr && task) override
    {
        {
            std::lock_guard lock(mu);
            if (!finished)
            {
                task->setNotifyType(notify_type);
                cv.registerTask(std::move(task));
                return;
            }
        }
        PipeConditionVariable::notifyTaskDirectly(std::move(task));
    }

    void finish()
    {
        {
            std::lock_guard lock(mu);
            if (finished)
                return;
            finished = true;
        }
        cv.notifyAll();
    }

private:
    std::mutex mu;
    bool finished{false};
    PipeConditionVariable cv;
    NotifyType notify_type;
};
using OneTimeNotifyFuturePtr = std::shared_ptr<OneTimeNotifyFuture>;
} // namespace DB
