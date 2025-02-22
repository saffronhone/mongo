/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/cost_based_ranker/heuristic_estimator.h"

#include <limits>

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::cost_based_ranker {
namespace {

CardinalityEstimate makeCard(double d) {
    return CardinalityEstimate(CardinalityType(d), EstimationSource::Code);
}

SelectivityEstimate makeSel(double d) {
    return SelectivityEstimate(SelectivityType(d), EstimationSource::Code);
}

std::unique_ptr<MatchExpression> parse(const BSONObj& bson) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto expr = MatchExpressionParser::parse(
        bson, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(expr);
    return std::move(expr.getValue());
}

TEST(HeuristicEstimator, AlwaysFalse) {
    BSONObj query = fromjson("{$alwaysFalse: 1}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), zeroSel);
}

TEST(HeuristicEstimator, AlwaysTrue) {
    BSONObj query = fromjson("{$alwaysTrue: 1}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), oneSel);
}

TEST(HeuristicEstimate, RegexMatchExpression) {
    BSONObj query = fromjson("{a: /abc/}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), kRegexSel);
}

TEST(HeuristicEstimate, ModExpression) {
    BSONObj query = fromjson("{a: {$mod: [5, 2]}}");
    auto expr = parse(query);
    // Selectivty of mod 5 is 1/5
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), makeSel(0.2));
}

TEST(HeuristicEstimate, ExistsExpression) {
    // Note: {$exists: false} is parsed as {$not: {$exists: true}}
    BSONObj query = fromjson("{a: {$exists: true}}");
    auto expr = parse(query);
    ASSERT_EQ(estimateLeafMatchExpression(expr.get(), makeCard(100)), kExistsSel);
}

TEST(HeuristicEstimate, BitsExpression) {
    BSONObj bitsAllClearQuery = fromjson("{a: {$bitsAllClear: [1, 5]}}");
    auto bitsAllClearExpr = parse(bitsAllClearQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsAllClearExpr.get(), makeCard(100)), kBitsSel);

    BSONObj bitsAllSetQuery = fromjson("{a: {$bitsAllSet: [1, 5]}}");
    auto bitsSetClearExpr = parse(bitsAllSetQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsSetClearExpr.get(), makeCard(100)), kBitsSel);

    BSONObj bitsAnyClearQuery = fromjson("{a: {$bitsAnyClear: [1, 5]}}");
    auto bitsAnyClearExpr = parse(bitsAnyClearQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsAnyClearExpr.get(), makeCard(100)), kBitsSel);

    BSONObj bitsAnySetQuery = fromjson("{a: {$bitsAnySet: [1, 5]}}");
    auto bitsAnySetExpr = parse(bitsAnySetQuery);
    ASSERT_EQ(estimateLeafMatchExpression(bitsAnySetExpr.get(), makeCard(100)), kBitsSel);
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
