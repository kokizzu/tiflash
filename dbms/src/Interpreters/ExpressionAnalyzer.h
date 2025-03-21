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
#include <Interpreters/AggregateDescription.h>
#include <Interpreters/Settings.h>
#include <Interpreters/SubqueryForSet.h>


namespace DB
{
class Context;

class ExpressionActions;
struct ExpressionActionsChain;

using PreparedSets = std::unordered_map<IAST *, SetPtr>;

using Tables = std::map<String, StoragePtr>;

class ASTFunction;
class ASTExpressionList;
class ASTSelectQuery;


/** Transforms an expression from a syntax tree into a sequence of actions to execute it.
  *
  * NOTE: if `ast` is a SELECT query from a table, the structure of this table should not change during the lifetime of ExpressionAnalyzer.
  */
class ExpressionAnalyzer : private boost::noncopyable
{
private:
    using ExpressionActionsPtr = std::shared_ptr<ExpressionActions>;

public:
    ExpressionAnalyzer(
        const ASTPtr & ast_,
        const Context & context_,
        const StoragePtr & storage_,
        const NamesAndTypesList & source_columns_ = {},
        const Names & required_result_columns_ = {},
        size_t subquery_depth_ = 0,
        bool do_global_ = false,
        const SubqueriesForSets & subqueries_for_set_ = {});

    /// Does the expression have aggregate functions or a GROUP BY or HAVING section.
    bool hasAggregation() const { return has_aggregation; }

    /// Get a list of aggregation keys and descriptions of aggregate functions if the query contains GROUP BY.
    void getAggregateInfo(Names & key_names, AggregateDescriptions & aggregates) const;

    /** Get a set of columns that are enough to read from the table to evaluate the expression.
      * Columns added from another table by JOIN are not counted.
      */
    Names getRequiredSourceColumns() const;

    /** These methods allow you to build a chain of transformations over a block, that receives values in the desired sections of the query.
      *
      * Example usage:
      *   ExpressionActionsChain chain;
      *   analyzer.appendWhere(chain);
      *   chain.addStep();
      *   analyzer.appendSelect(chain);
      *   analyzer.appendOrderBy(chain);
      *   chain.finalize();
      *
      * If only_types = true set, does not execute subqueries in the relevant parts of the query. The actions got this way
      *  shouldn't be executed, they are only needed to get a list of columns with their types.
      */

    /// Before aggregation:
    bool appendJoin(ExpressionActionsChain & chain, bool only_types);
    bool appendWhere(ExpressionActionsChain & chain, bool only_types);
    bool appendGroupBy(ExpressionActionsChain & chain, bool only_types);
    void appendAggregateFunctionsArguments(ExpressionActionsChain & chain, bool only_types);

    /// After aggregation:
    bool appendHaving(ExpressionActionsChain & chain, bool only_types);
    void appendSelect(ExpressionActionsChain & chain, bool only_types);
    bool appendOrderBy(ExpressionActionsChain & chain, bool only_types);
    bool appendLimitBy(ExpressionActionsChain & chain, bool only_types);
    /// Deletes all columns except mentioned by SELECT, arranges the remaining columns and renames them to aliases.
    void appendProjectResult(ExpressionActionsChain & chain) const;

    /// If `ast` is not a SELECT query, just gets all the actions to evaluate the expression.
    /// If project_result, only the calculated values in the desired order, renamed to aliases, remain in the output block.
    /// Otherwise, only temporary columns will be deleted from the block.
    ExpressionActionsPtr getActions(bool project_result);

    /// Actions that can be performed on an empty block: adding constants and applying functions that depend only on constants.
    /// Does not execute subqueries.
    ExpressionActionsPtr getConstActions();

    /** Sets that require a subquery to be create.
      * Only the sets needed to perform actions returned from already executed `append*` or `getActions`.
      * That is, you need to call getSetsWithSubqueries after all calls of `append*` or `getActions`
      *  and create all the returned sets before performing the actions.
      */
    SubqueriesForSets getSubqueriesForSets() const { return subqueries_for_sets; }

    PreparedSets getPreparedSets() { return prepared_sets; }

private:
    ASTPtr ast;
    ASTSelectQuery * select_query;
    const Context & context;
    Settings settings;
    size_t subquery_depth;

    /** Original columns.
      * First, all available columns of the table are placed here. Then (when analyzing the query), unused columns are deleted.
      */
    NamesAndTypesList source_columns;

    /** If non-empty, ignore all expressions in  not from this list.
      */
    NameSet required_result_columns;

    /// Columns after JOIN, and/or aggregation.
    NamesAndTypesList aggregated_columns;

    /// The main table in FROM clause, if exists.
    StoragePtr storage;

    bool has_aggregation = false;
    NamesAndTypesList aggregation_keys;
    AggregateDescriptions aggregate_descriptions;

    SubqueriesForSets subqueries_for_sets;

    PreparedSets prepared_sets;

    /// NOTE: So far, only one JOIN per query is supported.

    /** Query of the form `SELECT expr(x) AS FROM t1 ANY LEFT JOIN (SELECT expr(x) AS k FROM t2) USING k`
      * The join is made by column k.
      * During the JOIN,
      *  - in the "right" table, it will be available by alias `k`, since `Project` action for the subquery was executed.
      *  - in the "left" table, it will be accessible by the name `expr(x)`, since `Project` action has not been executed yet.
      * You must remember both of these options.
      */
    Names join_key_names_left;
    Names join_key_names_right;

    NamesAndTypesList columns_added_by_join;

    using Aliases = std::unordered_map<String, ASTPtr>;
    Aliases aliases;

    using SetOfASTs = std::set<const IAST *>;
    using MapOfASTs = std::map<ASTPtr, ASTPtr>;

    /** Remove all unnecessary columns from the list of all available columns of the table (`columns`).
      * At the same time, form a set of unknown columns (`unknown_required_source_columns`),
      * as well as the columns added by JOIN (`columns_added_by_join`).
      */
    void collectUsedColumns();

    /** Find the columns that are obtained by JOIN.
      */
    void collectJoinedColumns(NameSet & joined_columns, NamesAndTypesList & joined_columns_name_type);

    /** Create a dictionary of aliases.
      */
    void addASTAliases(ASTPtr & ast, int ignore_levels = 0);

    /** For star nodes(`*`), expand them to a list of all columns.
      * For literal nodes, substitute aliases.
      */
    void normalizeTree();
    void normalizeTreeImpl(
        ASTPtr & ast,
        MapOfASTs & finished_asts,
        SetOfASTs & current_asts,
        std::string current_alias,
        size_t level);

    ///    Eliminates injective function calls and constant expressions from group by statement
    void optimizeGroupBy();

    /// Remove duplicate items from ORDER BY.
    void optimizeOrderBy();

    void optimizeLimitBy();

    /// remove Function_if AST if condition is constant
    void optimizeIfWithConstantCondition();
    void optimizeIfWithConstantConditionImpl(ASTPtr & current_ast, Aliases & aliases) const;
    bool tryExtractConstValueFromCondition(const ASTPtr & condition, bool & value) const;

    void makeSet(const ASTFunction * node, const Block & sample_block);

    /// Adds a list of ALIAS columns from the table.
    void addAliasColumns();

    /// Replacing scalar subqueries with constant values.
    void executeScalarSubqueries();
    void executeScalarSubqueriesImpl(ASTPtr & ast);

    void addJoinAction(ExpressionActionsPtr & actions, bool only_types) const;

    struct ScopeStack;
    void getActionsImpl(const ASTPtr & ast, bool no_subqueries, bool only_consts, ScopeStack & actions_stack);

    void getRootActions(const ASTPtr & ast, bool no_subqueries, bool only_consts, ExpressionActionsPtr & actions);

    void getActionsBeforeAggregation(const ASTPtr & ast, ExpressionActionsPtr & actions, bool no_subqueries);

    /** Add aggregation keys to aggregation_keys, aggregate functions to aggregate_descriptions,
      * Create a set of columns aggregated_columns resulting after the aggregation, if any,
      *  or after all the actions that are normally performed before aggregation.
      * Set has_aggregation = true if there is GROUP BY or at least one aggregate function.
      */
    void analyzeAggregation();
    void getAggregates(const ASTPtr & ast, ExpressionActionsPtr & actions);
    void assertNoAggregates(const ASTPtr & ast, const char * description);

    /** Get a set of necessary columns to read from the table.
      * In this case, the columns specified in ignored_names are considered unnecessary. And the ignored_names parameter can be modified.
      * The set of columns available_joined_columns are the columns available from JOIN, they are not needed for reading from the main table.
      * Put in required_joined_columns the set of columns available from JOIN and needed.
      */
    void getRequiredSourceColumnsImpl(
        const ASTPtr & ast,
        const NameSet & available_columns,
        NameSet & required_source_columns,
        NameSet & ignored_names,
        const NameSet & available_joined_columns,
        NameSet & required_joined_columns);

    /// columns - the columns that are present before the transformations begin.
    static void initChain(ExpressionActionsChain & chain, const NamesAndTypesList & columns);

    void assertSelect() const;
    void assertAggregation() const;

    /** Create Set from an explicit enumeration of values in the query.
      * If create_ordered_set = true - create a data structure suitable for using the index.
      */
    void makeExplicitSet(const ASTFunction * node, const Block & sample_block, bool create_ordered_set);

    /**
      * Create Set from a subquery or a table expression in the query. The created set is suitable for using the index.
      * The set will not be created if its size hits the limit.
      */
    void tryMakeSetFromSubquery(const ASTPtr & subquery_or_table_name);

    /** Translate qualified names such as db.table.column, table.column, table_alias.column
      *  to unqualified names. This is done in a poor transitional way:
      *  only one ("main") table is supported. Ambiguity is not detected or resolved.
      */
    void translateQualifiedNames();
    void translateQualifiedNamesImpl(
        ASTPtr & ast,
        const String & database_name,
        const String & table_name,
        const String & alias);

    /** Sometimes we have to calculate more columns in SELECT clause than will be returned from query.
      * This is the case when we have DISTINCT : we require more columns in SELECT even if we need less columns in result.
      */
    void removeUnneededColumnsFromSelectClause();
};

} // namespace DB
