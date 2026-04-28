#pragma once

namespace gma {

/**
 * Register all builtin functions into FunctionMap.
 * TreeBuilder resolves Worker "fn" names through this registry, so any
 * function registered here is available as a Worker operation.
 *
 * Categories:
 *   Aggregation : mean/avg, sum, product, min, max, first, last, count,
 *                 median, range, stddev, variance
 *   Binary      : diff, spread, div, mod, pow, midpoint, pct_change, ratio
 *   Unary       : abs, neg, reciprocal, sqrt, cbrt, exp, log, log2, log10,
 *                 ceil, floor, round, trunc, sign
 *   Trig        : sin, cos, tan, asin, acos, atan, atan2
 *   Comparison  : gt, lt, gte, lte, eq, neq
 *   Logical     : and, or, not
 *   Financial   : zscore, ema_weight, cumulative_return
 */
void registerBuiltinFunctions();

} // namespace gma
