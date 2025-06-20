// Copyright 2025 PingCAP, Ltd.
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

#include <Common/Decimal.h>
#include <Interpreters/Context.h>
#include <Interpreters/WindowDescription.h>
#include <TestUtils/ExecutorTestUtils.h>
#include <TestUtils/FunctionTestUtils.h>
#include <TestUtils/WindowTestUtils.h>
#include <TestUtils/mockExecutor.h>
#include <common/types.h>
#include <gtest/gtest.h>
#include <tipb/executor.pb.h>

#include <optional>


namespace DB::tests
{
struct TestCase
{
    TestCase(
        const ASTPtr & ast_func_,
        const std::vector<Int64> & start_offsets_,
        const std::vector<Int64> & end_offsets_,
        const std::vector<std::vector<std::optional<Int64>>> & results_,
        const std::vector<std::vector<std::optional<Float64>>> & float_results_,
        bool is_range_frame_,
        bool is_input_value_nullable_,
        bool is_return_type_int_ = true)
        : ast_func(ast_func_)
        , start_offsets(start_offsets_)
        , end_offsets(end_offsets_)
        , results(results_)
        , float_results(float_results_)
        , is_range_frame(is_range_frame_)
        , is_input_value_nullable(is_input_value_nullable_)
        , is_return_type_int(is_return_type_int_)
        , test_case_num(start_offsets.size())
    {
        assert(test_case_num == end_offsets.size());
        assert((test_case_num == results.size()) || (test_case_num == float_results.size()));
    }

    ASTPtr ast_func;
    std::vector<Int64> start_offsets;
    std::vector<Int64> end_offsets;
    std::vector<std::vector<std::optional<Int64>>> results;
    std::vector<std::vector<std::optional<Float64>>> float_results;
    bool is_range_frame;
    bool is_input_value_nullable;
    bool is_return_type_int;
    size_t test_case_num;
};

class WindowAggFuncTest : public DB::tests::WindowTest
{
public:
    const ASTPtr value_col = col(VALUE_COL_NAME);

    void initializeContext() override { ExecutorTest::initializeContext(); }

    void executeTest(const TestCase & test);
    void executeTestForIssue9913(const TestCase & test);

protected:
    static std::vector<Int64> partition;
    static std::vector<Int64> order;
    static std::vector<Int64> int_value;
    static std::vector<std::optional<Int64>> int_nullable_value;
};

std::vector<Int64> WindowAggFuncTest::partition = {0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 6, 6};
std::vector<Int64> WindowAggFuncTest::order = {0, 0, 1, 3, 6, 0, 1, 4, 7, 8, 0, 4, 6, 10, 20, 40, 41, 0, 0, 0, 10, 30};
std::vector<Int64> WindowAggFuncTest::int_value
    = {0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0};
std::vector<std::optional<Int64>> WindowAggFuncTest::int_nullable_value
    = {0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0};

void WindowAggFuncTest::executeTest(const TestCase & test)
{
    MockWindowFrame frame;
    if (test.is_range_frame)
        frame.type = tipb::WindowFrameType::Ranges;
    else
        frame.type = tipb::WindowFrameType::Rows;

    for (size_t i = 0; i < test.test_case_num; i++)
    {
        if (test.is_range_frame)
        {
            frame.start = buildRangeFrameBound<Int64>(
                tipb::WindowBoundType::Preceding,
                tipb::RangeCmpDataType::Int,
                ORDER_COL_NAME,
                false,
                test.start_offsets[i]);
            frame.end = buildRangeFrameBound<Int64>(
                tipb::WindowBoundType::Following,
                tipb::RangeCmpDataType::Int,
                ORDER_COL_NAME,
                true,
                test.end_offsets[i]);
        }
        else
        {
            frame.start = mock::MockWindowFrameBound(tipb::WindowBoundType::Preceding, false, test.start_offsets[i]);
            frame.end = mock::MockWindowFrameBound(tipb::WindowBoundType::Following, false, test.end_offsets[i]);
        }

        ColumnWithTypeAndName value_col_with_type_and_name;
        if (test.is_input_value_nullable)
            value_col_with_type_and_name = toNullableVec<Int64>(int_nullable_value);
        else
            value_col_with_type_and_name = toVec<Int64>(int_value);

        ColumnWithTypeAndName res;
        if (test.is_return_type_int)
            res = toNullableVec<Int64>(test.results[i]);
        else
            res = toNullableVec<Float64>(test.float_results[i]);

        executeFunctionAndAssert(
            res,
            test.ast_func,
            {toVec<Int64>(partition), toVec<Int64>(order), value_col_with_type_and_name},
            frame);
    }
}

void WindowAggFuncTest::executeTestForIssue9913(const TestCase & test)
{
    MockWindowFrame frame;
    if (test.is_range_frame)
        frame.type = tipb::WindowFrameType::Ranges;
    else
        frame.type = tipb::WindowFrameType::Rows;

    frame.start = mock::MockWindowFrameBound(tipb::WindowBoundType::Preceding, true, 0);
    frame.end = mock::MockWindowFrameBound(tipb::WindowBoundType::Following, true, 0);

    ColumnWithTypeAndName value_col_with_type_and_name;
    if (test.is_input_value_nullable)
        value_col_with_type_and_name = toNullableVec<Int64>(int_nullable_value);
    else
        value_col_with_type_and_name = toVec<Int64>(int_value);

    ColumnWithTypeAndName res;
    if (test.is_return_type_int)
        res = toNullableVec<Int64>(test.results[0]);
    else
        res = toNullableVec<Float64>(test.float_results[0]);

    executeFunctionAndAssert(
        res,
        test.ast_func,
        {toVec<Int64>(partition), toVec<Int64>(order), value_col_with_type_and_name},
        frame);
}


// TODO test duplicate order by values in range frame in ft
TEST_F(WindowAggFuncTest, Sum)
try
{
    std::vector<Int64> start_offset{0, 1, 3, 10, 0, 0, 0, 3};
    std::vector<Int64> end_offset{0, 0, 0, 0, 1, 3, 10, 3};
    std::vector<std::vector<std::optional<Int64>>> res;
    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, 4, 10, 2, 2, -4, -6, -1, 7, 4, 6, 0, -12, -1, 3, 4, -5, 2, 7, 5},
           {0, -1, -1, 3, 9, 2, 2, -2, -4, -5, 7, 4, 13, 4, -6, -1, -9, 4, -5, 2, 7, 7},
           {0, -1, -1, 3, 9, 2, 2, -2, -4, -3, 7, 4, 13, 4, 1, 3, 4, 4, -5, 2, 7, 7},
           {0, -1, 4, 10, 6, 2, -4, -6, -1, 1, 4, 6, 0, -12, -1, 3, 1, 4, -5, 7, 5, 0},
           {0, 9, 10, 10, 6, -4, -5, -5, -1, 1, 4, -6, -1, -9, 0, 3, 1, 4, -5, 7, 5, 0},
           {0, 9, 10, 10, 6, -3, -5, -5, -1, 1, 4, -3, 0, -9, 0, 3, 1, 4, -5, 7, 5, 0},
           {0, 9, 9, 9, 9, -4, -3, -3, -3, -5, 4, 1, 3, 4, -3, 0, -9, 4, -5, 7, 7, 7}};

    executeTest(TestCase(Sum(value_col), start_offset, end_offset, res, {}, false, false));
    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, 4, 6, 2, 2, -4, -2, -1, 7, -3, 9, -9, -3, 2, 3, 4, -5, 2, 5, 0},
           {0, -1, -1, 3, 10, 2, 2, -4, -6, -1, 7, -3, 6, -9, -3, 2, 3, 4, -5, 2, 5, 0},
           {0, -1, -1, 3, 9, 2, 2, -2, -4, -3, 7, 4, 13, 4, -12, 2, 3, 4, -5, 2, 7, 0},
           {0, -1, 0, 4, 6, 2, 0, -4, -1, 1, 7, -3, 9, -9, -3, 3, 1, 4, -5, 2, 5, 0},
           {0, 3, 4, 10, 6, 2, -4, -6, -1, 1, 7, 6, 9, -9, -3, 3, 1, 4, -5, 2, 5, 0},
           {0, 9, 10, 10, 6, -3, -5, -5, -1, 1, 4, -3, 0, -12, -3, 3, 1, 4, -5, 7, 5, 0},
           {0, 3, 3, 9, 10, 2, -2, -6, -5, -1, 7, 6, 6, -9, -3, 3, 3, 4, -5, 2, 5, 0}};
    executeTest(TestCase(Sum(value_col), start_offset, end_offset, res, {}, true, false));
    res
        = {{0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, {}, 6, 2, 2, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, 6, 2, 2, -4, -4, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, 5, 2, 2, -2, -2, -2, 7, 7, 16, 16, {}, {}, {}, 4, {}, 2, 2, 0},
           {0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, {}, 6, 6, 2, -4, -4, {}, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, 5, 6, 6, 6, -2, -4, -4, {}, {}, 16, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, 5, 6, 2, -2, -4, -4, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0}};
    executeTest(TestCase(Sum(value_col), start_offset, end_offset, res, {}, true, true));
}
CATCH

TEST_F(WindowAggFuncTest, Count)
try
{
    std::vector<Int64> start_offset{0, 1, 3, 10, 0, 0, 0, 3};
    std::vector<Int64> end_offset{0, 0, 0, 0, 1, 3, 10, 3};
    std::vector<std::vector<std::optional<Int64>>> res;

    res
        = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
           {1, 1, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2},
           {1, 1, 2, 3, 4, 1, 2, 3, 4, 4, 1, 2, 3, 4, 4, 4, 4, 1, 1, 1, 2, 3},
           {1, 1, 2, 3, 4, 1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 6, 7, 1, 1, 1, 2, 3},
           {1, 2, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 1},
           {1, 4, 3, 2, 1, 4, 4, 3, 2, 1, 4, 4, 4, 4, 3, 2, 1, 1, 1, 3, 2, 1},
           {1, 4, 3, 2, 1, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 1, 1, 3, 2, 1},
           {1, 4, 4, 4, 4, 4, 5, 5, 5, 4, 4, 5, 6, 7, 6, 5, 4, 1, 1, 3, 3, 3}};

    executeTest(TestCase(Count(value_col), start_offset, end_offset, res, {}, false, false));

    res
        = {{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
           {1, 1, 2, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1},
           {1, 1, 2, 3, 2, 1, 2, 2, 2, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 1},
           {1, 1, 2, 3, 4, 1, 2, 3, 4, 5, 1, 2, 3, 4, 2, 1, 2, 1, 1, 1, 2, 1},
           {1, 2, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1},
           {1, 3, 2, 2, 1, 2, 2, 2, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1},
           {1, 4, 3, 2, 1, 5, 4, 3, 2, 1, 4, 3, 2, 2, 1, 2, 1, 1, 1, 2, 1, 1},
           {1, 3, 3, 4, 2, 2, 3, 3, 3, 2, 1, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 1}};
    executeTest(TestCase(Count(value_col), start_offset, end_offset, res, {}, true, false));

    res
        = {{1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1},
           {1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1},
           {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1},
           {1, 1, 1, 1, 2, 1, 1, 2, 2, 2, 1, 1, 2, 2, 0, 0, 0, 1, 0, 1, 1, 1},
           {1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1},
           {1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1},
           {1, 2, 1, 1, 1, 2, 1, 1, 0, 0, 2, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1},
           {1, 1, 1, 2, 1, 1, 2, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1}};
    executeTest(TestCase(Count(value_col), start_offset, end_offset, res, {}, true, true));
}
CATCH

TEST_F(WindowAggFuncTest, Avg)
try
{
    std::vector<Int64> start_offset;
    std::vector<Int64> end_offset;
    std::vector<std::vector<std::optional<Float64>>> res;

    start_offset = {0, 1, 0};
    end_offset = {0, 0, 1};
    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -0.5, 2, 5, 2, 1, -2, -3, -0.5, 7, 2, 3, 0, -6, -0.5, 1.5, 4, -5, 2, 3.5, 2.5},
           {0, -0.5, 2, 5, 6, 1, -2, -3, -0.5, 1, 2, 3, 0, -6, -0.5, 1.5, 1, 4, -5, 3.5, 2.5, 0}};

    executeTest(TestCase(Avg(value_col), start_offset, end_offset, {}, res, false, false, false));
}
CATCH

TEST_F(WindowAggFuncTest, Min)
try
{
    std::vector<Int64> start_offset{0, 1, 3, 10, 0, 0, 0, 3};
    std::vector<Int64> end_offset{0, 0, 0, 0, 1, 3, 10, 3};
    std::vector<std::vector<std::optional<Int64>>> res;

    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, 0, 4, 2, 0, -4, -4, -2, 7, -3, -3, -9, -9, -3, 1, 4, -5, 2, 2, 0},
           {0, -1, -1, -1, -1, 2, 0, -4, -4, -4, 7, -3, -3, -9, -9, -9, -9, 4, -5, 2, 2, 0},
           {0, -1, -1, -1, -1, 2, 0, -4, -4, -4, 7, -3, -3, -9, -9, -9, -9, 4, -5, 2, 2, 0},
           {0, -1, 0, 4, 6, 0, -4, -4, -2, 1, -3, -3, -9, -9, -3, 1, 1, 4, -5, 2, 0, 0},
           {0, -1, 0, 4, 6, -4, -4, -4, -2, 1, -9, -9, -9, -9, -3, 1, 1, 4, -5, 0, 0, 0},
           {0, -1, 0, 4, 6, -4, -4, -4, -2, 1, -9, -9, -9, -9, -3, 1, 1, 4, -5, 0, 0, 0},
           {0, -1, -1, -1, -1, -4, -4, -4, -4, -4, -9, -9, -9, -9, -9, -9, -9, 4, -5, 0, 0, 0}};
    executeTest(TestCase(MinForWindow(value_col), start_offset, end_offset, res, {}, false, false));

    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, 4, 6, 2, 0, -4, -2, -2, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, -1, 4, 2, 0, -4, -4, -2, 7, -3, -3, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, -1, -1, 2, 0, -4, -4, -4, 7, -3, -3, -9, -9, 2, 1, 4, -5, 2, 2, 0},
           {0, -1, 0, 4, 6, 0, 0, -4, -2, 1, 7, -3, 9, -9, -3, 1, 1, 4, -5, 2, 5, 0},
           {0, -1, 0, 4, 6, 0, -4, -4, -2, 1, 7, -3, 9, -9, -3, 1, 1, 4, -5, 2, 5, 0},
           {0, -1, 0, 4, 6, -4, -4, -4, -2, 1, -9, -9, -9, -9, -3, 1, 1, 4, -5, 2, 5, 0},
           {0, -1, -1, -1, 4, 0, -4, -4, -4, -2, 7, -3, -3, -9, -3, 1, 1, 4, -5, 2, 5, 0}};
    executeTest(TestCase(MinForWindow(value_col), start_offset, end_offset, res, {}, true, false));

    res
        = {{0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, {}, 6, 2, 2, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, 6, 2, 2, -4, -4, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, -1, 2, 2, -4, -4, -4, 7, 7, 7, 7, {}, {}, {}, 4, {}, 2, 2, 0},
           {0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, {}, 6, 6, 2, -4, -4, {}, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, 6, 6, 6, -4, -4, -4, {}, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, 6, 2, -4, -4, -4, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0}};
    executeTest(TestCase(MinForWindow(value_col), start_offset, end_offset, res, {}, true, true));
}
CATCH

TEST_F(WindowAggFuncTest, Max)
try
{
    std::vector<Int64> start_offset{0, 1, 3, 10, 0, 0, 0, 3};
    std::vector<Int64> end_offset{0, 0, 0, 0, 1, 3, 10, 3};
    std::vector<std::vector<std::optional<Int64>>> res;

    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, 0, 4, 6, 2, 2, 0, -2, 1, 7, 7, 9, 9, -3, 2, 2, 4, -5, 2, 5, 5},
           {0, -1, 0, 4, 6, 2, 2, 2, 2, 1, 7, 7, 9, 9, 9, 9, 2, 4, -5, 2, 5, 5},
           {0, -1, 0, 4, 6, 2, 2, 2, 2, 2, 7, 7, 9, 9, 9, 9, 9, 4, -5, 2, 5, 5},
           {0, 0, 4, 6, 6, 2, 0, -2, 1, 1, 7, 9, 9, -3, 2, 2, 1, 4, -5, 5, 5, 0},
           {0, 6, 6, 6, 6, 2, 1, 1, 1, 1, 9, 9, 9, 2, 2, 2, 1, 4, -5, 5, 5, 0},
           {0, 6, 6, 6, 6, 2, 1, 1, 1, 1, 9, 9, 9, 2, 2, 2, 1, 4, -5, 5, 5, 0},
           {0, 6, 6, 6, 6, 2, 2, 2, 2, 1, 9, 9, 9, 9, 9, 9, 2, 4, -5, 5, 5, 5}};
    executeTest(TestCase(MaxForWindow(value_col), start_offset, end_offset, res, {}, false, false));

    res
        = {{0, -1, 0, 4, 6, 2, 0, -4, -2, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, -1, 0, 4, 6, 2, 2, -4, -2, 1, 7, -3, 9, -9, -3, 2, 2, 4, -5, 2, 5, 0},
           {0, -1, 0, 4, 6, 2, 2, 0, -2, 1, 7, -3, 9, -9, -3, 2, 2, 4, -5, 2, 5, 0},
           {0, -1, 0, 4, 6, 2, 2, 2, 2, 2, 7, 7, 9, 9, -3, 2, 2, 4, -5, 2, 5, 0},
           {0, 0, 0, 4, 6, 2, 0, -4, 1, 1, 7, -3, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, 4, 4, 6, 6, 2, 0, -2, 1, 1, 7, 9, 9, -9, -3, 2, 1, 4, -5, 2, 5, 0},
           {0, 6, 6, 6, 6, 2, 1, 1, 1, 1, 9, 9, 9, -3, -3, 2, 1, 4, -5, 5, 5, 0},
           {0, 4, 4, 6, 6, 2, 2, 0, 1, 1, 7, 9, 9, -9, -3, 2, 2, 4, -5, 2, 5, 0}};
    executeTest(TestCase(MaxForWindow(value_col), start_offset, end_offset, res, {}, true, false));

    res
        = {{0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, {}, 6, 2, 2, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, 6, 2, 2, -4, -4, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, -1, 6, 2, 2, 2, 2, 2, 7, 7, 9, 9, {}, {}, {}, 4, {}, 2, 2, 0},
           {0, -1, {}, {}, 6, 2, {}, -4, {}, {}, 7, {}, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, {}, 6, 6, 2, -4, -4, {}, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, 6, 6, 6, 6, 2, -4, -4, {}, {}, 9, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0},
           {0, -1, -1, 6, 6, 2, 2, -4, -4, {}, 7, 9, 9, {}, {}, {}, {}, 4, {}, 2, {}, 0}};
    executeTest(TestCase(MaxForWindow(value_col), start_offset, end_offset, res, {}, true, true));
}
CATCH

tipb::WindowFrameBound makeTiPBWindowBound(WindowFrame::BoundaryType type, UInt64 offset, bool preceding)
{
    tipb::WindowFrameBound bound;
    bound.set_unbounded(false);
    if (preceding)
        bound.set_type(tipb::WindowBoundType::Preceding);
    else
        bound.set_type(tipb::WindowBoundType::Following);

    switch (type)
    {
    case WindowFrame::BoundaryType::Current:
        bound.set_type(tipb::WindowBoundType::CurrentRow);
        break;
    case WindowFrame::BoundaryType::Offset:
        bound.set_offset(offset);
        break;
    case WindowFrame::BoundaryType::Unbounded:
        bound.set_unbounded(true);
        break;
    }
    return bound;
}

tipb::WindowFrame makeTiPBWindowFrame(
    const WindowFrame::FrameType type,
    const WindowFrame::BoundaryType begin_type,
    const UInt64 begin_offset,
    const bool begin_preceding,
    const WindowFrame::BoundaryType end_type,
    const UInt64 end_offset,
    const bool end_preceding)
{
    tipb::WindowFrame frame;
    switch (type)
    {
    case WindowFrame::FrameType::Rows:
        frame.set_type(tipb::WindowFrameType::Rows);
        break;
    case WindowFrame::FrameType::Ranges:
        frame.set_type(tipb::WindowFrameType::Ranges);
        break;
    default:
        throw Exception("Invalid frame type");
    }
    (*frame.mutable_start()) = makeTiPBWindowBound(begin_type, begin_offset, begin_preceding);
    (*frame.mutable_end()) = makeTiPBWindowBound(end_type, end_offset, end_preceding);
    return frame;
}

WindowFrame makeWindowFrame(
    const WindowFrame::FrameType type,
    const WindowFrame::BoundaryType begin_type,
    const UInt64 begin_offset,
    const bool begin_preceding,
    const WindowFrame::BoundaryType end_type,
    const UInt64 end_offset,
    const bool end_preceding)
{
    WindowFrame frame;
    setWindowFrameImpl(
        frame,
        makeTiPBWindowFrame(type, begin_type, begin_offset, begin_preceding, end_type, end_offset, end_preceding));
    return frame;
}

TEST_F(WindowAggFuncTest, initNeedDecrease)
try
{
    WindowDescription desc;
    desc.frame = WindowFrame();
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    tipb::WindowFrame w;
    tipb::WindowFrameBound b;

    std::vector<WindowFrame::FrameType> frame_types;
    for (auto type : frame_types)
    {
        desc.frame = makeWindowFrame(
            type,
            WindowFrame::BoundaryType::Unbounded,
            0,
            true,
            WindowFrame::BoundaryType::Offset,
            1,
            false);
        desc.initNeedDecrease(true);
        ASSERT_FALSE(desc.need_decrease);

        desc.frame = makeWindowFrame(
            type,
            WindowFrame::BoundaryType::Offset,
            0,
            true,
            WindowFrame::BoundaryType::Unbounded,
            0,
            false);
        desc.initNeedDecrease(true);
        ASSERT_TRUE(desc.need_decrease);

        desc.frame = makeWindowFrame(
            type,
            WindowFrame::BoundaryType::Unbounded,
            0,
            true,
            WindowFrame::BoundaryType::Unbounded,
            0,
            false);
        desc.initNeedDecrease(true);
        ASSERT_FALSE(desc.need_decrease);

        desc.frame = makeWindowFrame(
            type,
            WindowFrame::BoundaryType::Current,
            0,
            true,
            WindowFrame::BoundaryType::Unbounded,
            0,
            false);
        desc.initNeedDecrease(true);
        ASSERT_TRUE(desc.need_decrease);

        desc.frame = makeWindowFrame(
            type,
            WindowFrame::BoundaryType::Unbounded,
            0,
            true,
            WindowFrame::BoundaryType::Current,
            0,
            false);
        desc.initNeedDecrease(true);
        ASSERT_FALSE(desc.need_decrease);

        desc.frame = makeWindowFrame(
            type,
            WindowFrame::BoundaryType::Current,
            0,
            true,
            WindowFrame::BoundaryType::Current,
            0,
            false);
        desc.initNeedDecrease(true);
        ASSERT_FALSE(desc.need_decrease);
    }

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Ranges,
        WindowFrame::BoundaryType::Offset,
        0,
        true,
        WindowFrame::BoundaryType::Offset,
        0,
        true);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Ranges,
        WindowFrame::BoundaryType::Offset,
        0,
        false,
        WindowFrame::BoundaryType::Offset,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Ranges,
        WindowFrame::BoundaryType::Offset,
        0,
        true,
        WindowFrame::BoundaryType::Offset,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        0,
        true,
        WindowFrame::BoundaryType::Offset,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        0,
        true,
        WindowFrame::BoundaryType::Offset,
        0,
        true);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        0,
        false,
        WindowFrame::BoundaryType::Offset,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        3,
        true,
        WindowFrame::BoundaryType::Offset,
        3,
        true);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        3,
        false,
        WindowFrame::BoundaryType::Offset,
        3,
        false);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        3,
        true,
        WindowFrame::BoundaryType::Offset,
        3,
        false);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        2,
        true,
        WindowFrame::BoundaryType::Offset,
        1,
        false);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        0,
        true,
        WindowFrame::BoundaryType::Offset,
        1,
        false);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        1,
        true,
        WindowFrame::BoundaryType::Offset,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_TRUE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Current,
        0,
        true,
        WindowFrame::BoundaryType::Offset,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);

    desc.frame = makeWindowFrame(
        WindowFrame::FrameType::Rows,
        WindowFrame::BoundaryType::Offset,
        0,
        true,
        WindowFrame::BoundaryType::Current,
        0,
        false);
    desc.initNeedDecrease(true);
    ASSERT_FALSE(desc.need_decrease);
}
CATCH

TEST_F(WindowAggFuncTest, issue9868)
try
{
    MockWindowFrame mock_frame;
    mock_frame.type = tipb::WindowFrameType::Ranges;
    mock_frame.start = buildRangeFrameBound(
        tipb::WindowBoundType::Preceding,
        tipb::RangeCmpDataType::Int,
        ORDER_COL_NAME,
        false,
        static_cast<Int64>(1));
    mock_frame.end = mock::MockWindowFrameBound(tipb::WindowBoundType::CurrentRow, false, 0);

    std::vector<std::optional<Int64>> res{3, 1, 3,  23, 23, 23, 23, 23, 6, 6, 7, 4,  24, 24, 24, 24, 22, 3, 4,
                                          2, 6, 20, 20, 20, 18, 14, 14, 8, 8, 8, 14, 11, 14, 14, 14, 4,  5};
    auto partition_col = toVec<Int64>(/*partition*/ {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
                                                     3, 4, 4, 4, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6, 6, 6, 7, 8});
    auto order_col = toVec<Int64>(/*order*/ {0, 0, 1, 2, 2, 2, 2, 2, 4, 4, 5, 6, 7, 7, 7, 7, 8, 9, 0,
                                             0, 0, 1, 1, 1, 2, 3, 3, 0, 0, 0, 1, 2, 3, 3, 3, 0, 0});
    auto val_col = toVec<Int64>(/*value*/ {3, 1, 2, 4, 8, 5, 3, 1, 1, 5, 1, 3, 5, 9, 5, 2, 1, 2, 4,
                                           2, 6, 7, 4, 3, 4, 6, 4, 2, 6, 8, 6, 5, 4, 3, 2, 4, 5});

    executeFunctionAndAssert(
        toNullableVec<Int64>(res),
        Sum(value_col),
        {partition_col, order_col, val_col},
        mock_frame,
        false);
}
CATCH

TEST_F(WindowAggFuncTest, issue9913)
try
{
    std::vector<std::optional<Int64>> res
        = {0, -1, -1, -1, -1, -4, -4, -4, -4, -4, -9, -9, -9, -9, -9, -9, -9, 4, -5, 0, 0, 0};
    executeTestForIssue9913(TestCase(MinForWindow(value_col), {}, {}, {res}, {}, false, false));
    executeTestForIssue9913(TestCase(MinForWindow(value_col), {}, {}, {res}, {}, true, false));

    res = {0, 6, 6, 6, 6, 2, 2, 2, 2, 2, 9, 9, 9, 9, 9, 9, 9, 4, -5, 5, 5, 5};
    executeTestForIssue9913(TestCase(MaxForWindow(value_col), {}, {}, {res}, {}, false, false));
    executeTestForIssue9913(TestCase(MaxForWindow(value_col), {}, {}, {res}, {}, true, false));

    res = {0, 9, 9, 9, 9, -3, -3, -3, -3, -3, 4, 4, 4, 4, 4, 4, 4, 4, -5, 7, 7, 7};
    executeTestForIssue9913(TestCase(Sum(value_col), {}, {}, {res}, {}, false, false));
    executeTestForIssue9913(TestCase(Sum(value_col), {}, {}, {res}, {}, true, false));

    res = {1, 4, 4, 4, 4, 5, 5, 5, 5, 5, 7, 7, 7, 7, 7, 7, 7, 1, 1, 3, 3, 3};
    executeTestForIssue9913(TestCase(Count(value_col), {}, {}, {res}, {}, false, false));
    executeTestForIssue9913(TestCase(Count(value_col), {}, {}, {res}, {}, true, false));

    std::vector<std::optional<Float64>> float_res
        = {0,
           2.25,
           2.25,
           2.25,
           2.25,
           -0.6,
           -0.6,
           -0.6,
           -0.6,
           -0.6,
           0.5714285714285714,
           0.5714285714285714,
           0.5714285714285714,
           0.5714285714285714,
           0.5714285714285714,
           0.5714285714285714,
           0.5714285714285714,
           4,
           -5,
           2.3333333333333335,
           2.3333333333333335,
           2.3333333333333335};
    executeTestForIssue9913(TestCase(Avg(value_col), {}, {}, {}, {float_res}, false, false, false));
    executeTestForIssue9913(TestCase(Avg(value_col), {}, {}, {}, {float_res}, true, false, false));
}
CATCH

TEST_F(WindowAggFuncTest, issue10236)
try
{
    MockWindowFrame frame;
    frame.type = tipb::WindowFrameType::Ranges;
    frame.start = buildRangeFrameBound(
        tipb::WindowBoundType::Preceding,
        tipb::RangeCmpDataType::Int,
        ORDER_COL_NAME,
        false,
        static_cast<Int64>(0));

    std::vector<tipb::WindowBoundType> window_bound_types{
        tipb::WindowBoundType::Preceding,
        tipb::WindowBoundType::Following};

    for (auto window_bound_type : window_bound_types)
    {
        frame.end = buildRangeFrameBound(
            window_bound_type,
            tipb::RangeCmpDataType::Int,
            ORDER_COL_NAME,
            false,
            static_cast<Int64>(0));

        executeFunctionAndAssert(
            toNullableVec<Int64>(std::vector<std::optional<Int64>>{10, 10, 10, 10, 10, 10, 10, 10, 10, 10}),
            Count(value_col),
            {toVec<Int64>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
             toVec<Int64>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
             toVec<Int64>({0, 0, 0, 0, 0, 0, 0, 0, 0, 0})},
            frame);
    }
}
CATCH
} // namespace DB::tests
