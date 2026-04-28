// Generic, domain-free worker functions for FunctionMap. Stays in the engine.
#include "gma/FunctionRegistry.hpp"
#include "gma/FunctionMap.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gma {

// Minimum threshold for denominators to avoid division by near-zero values.
static constexpr double EPSILON = 1e-6;

void registerBuiltinFunctions() {
    auto& fm = FunctionMap::instance();

    // ──── Aggregation (full-vector reductions) ────

    fm.registerFunction("mean", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double s = 0.0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    });

    fm.registerFunction("avg", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double s = 0.0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    });

    fm.registerFunction("sum", [](const std::vector<double>& v) -> double {
        double s = 0.0;
        for (double x : v) s += x;
        return s;
    });

    fm.registerFunction("product", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double p = 1.0;
        for (double x : v) p *= x;
        return p;
    });

    fm.registerFunction("min", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double m = v[0];
        for (size_t i = 1; i < v.size(); ++i) m = std::min(m, v[i]);
        return m;
    });

    fm.registerFunction("max", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double m = v[0];
        for (size_t i = 1; i < v.size(); ++i) m = std::max(m, v[i]);
        return m;
    });

    fm.registerFunction("last", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : v.back();
    });

    fm.registerFunction("first", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : v.front();
    });

    fm.registerFunction("count", [](const std::vector<double>& v) -> double {
        return static_cast<double>(v.size());
    });

    fm.registerFunction("median", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        std::vector<double> sorted(v);
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();
        if (n % 2 == 1) return sorted[n / 2];
        return (sorted[n / 2 - 1] + sorted[n / 2]) * 0.5;
    });

    fm.registerFunction("range", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double lo = v[0], hi = v[0];
        for (size_t i = 1; i < v.size(); ++i) {
            lo = std::min(lo, v[i]);
            hi = std::max(hi, v[i]);
        }
        return hi - lo;
    });

    // Population standard deviation (divides by N, not N-1) — intentional for
    // consistency with the Bollinger Bands computation in computeAllAtomicValues().
    fm.registerFunction("stddev", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double n = static_cast<double>(v.size());
        double s = 0.0;
        for (double x : v) s += x;
        double mean = s / n;
        double ss = 0.0;
        for (double x : v) { double d = x - mean; ss += d * d; }
        return std::sqrt(ss / n);
    });

    fm.registerFunction("variance", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double n = static_cast<double>(v.size());
        double s = 0.0;
        for (double x : v) s += x;
        double mean = s / n;
        double ss = 0.0;
        for (double x : v) { double d = x - mean; ss += d * d; }
        return ss / n;
    });

    // ──── Binary ops (operate on first and last values) ────

    fm.registerFunction("diff", [](const std::vector<double>& v) -> double {
        return v.size() >= 2 ? v.back() - v.front() : 0.0;
    });

    fm.registerFunction("spread", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double lo = v[0], hi = v[0];
        for (size_t i = 1; i < v.size(); ++i) {
            lo = std::min(lo, v[i]);
            hi = std::max(hi, v[i]);
        }
        return hi - lo;
    });

    fm.registerFunction("div", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double denom = v.back();
        return std::abs(denom) > EPSILON ? v.front() / denom : 0.0;
    });

    fm.registerFunction("mod", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double denom = v.back();
        return std::abs(denom) > EPSILON ? std::fmod(v.front(), denom) : 0.0;
    });

    fm.registerFunction("pow", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        return std::pow(v.front(), v.back());
    });

    fm.registerFunction("midpoint", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return v.empty() ? 0.0 : v[0];
        return (v.front() + v.back()) * 0.5;
    });

    fm.registerFunction("pct_change", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double base = v.front();
        return std::abs(base) > EPSILON ? 100.0 * (v.back() - base) / base : 0.0;
    });

    fm.registerFunction("ratio", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double denom = v.back();
        return std::abs(denom) > EPSILON ? v.front() / denom : 0.0;
    });

    // ──── Unary ops (operate on last value) ────

    fm.registerFunction("abs", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::abs(v.back());
    });

    fm.registerFunction("neg", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : -v.back();
    });

    fm.registerFunction("reciprocal", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double x = v.back();
        return std::abs(x) > EPSILON ? 1.0 / x : 0.0;
    });

    fm.registerFunction("sqrt", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::sqrt(std::abs(v.back()));
    });

    fm.registerFunction("cbrt", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::cbrt(v.back());
    });

    fm.registerFunction("exp", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::exp(v.back());
    });

    fm.registerFunction("log", [](const std::vector<double>& v) -> double {
        if (v.empty() || v.back() <= 0.0) return 0.0;
        return std::log(v.back());
    });

    fm.registerFunction("log2", [](const std::vector<double>& v) -> double {
        if (v.empty() || v.back() <= 0.0) return 0.0;
        return std::log2(v.back());
    });

    fm.registerFunction("log10", [](const std::vector<double>& v) -> double {
        if (v.empty() || v.back() <= 0.0) return 0.0;
        return std::log10(v.back());
    });

    fm.registerFunction("ceil", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::ceil(v.back());
    });

    fm.registerFunction("floor", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::floor(v.back());
    });

    fm.registerFunction("round", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::round(v.back());
    });

    fm.registerFunction("trunc", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::trunc(v.back());
    });

    fm.registerFunction("sign", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double x = v.back();
        return (x > 0.0) ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
    });

    // ──── Trigonometric ────

    fm.registerFunction("sin", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::sin(v.back());
    });

    fm.registerFunction("cos", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::cos(v.back());
    });

    fm.registerFunction("tan", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::tan(v.back());
    });

    fm.registerFunction("asin", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double x = std::clamp(v.back(), -1.0, 1.0);
        return std::asin(x);
    });

    fm.registerFunction("acos", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double x = std::clamp(v.back(), -1.0, 1.0);
        return std::acos(x);
    });

    fm.registerFunction("atan", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : std::atan(v.back());
    });

    fm.registerFunction("atan2", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        return std::atan2(v.front(), v.back());
    });

    // ──── Comparison (return 1.0 for true, 0.0 for false) ────

    fm.registerFunction("gt", [](const std::vector<double>& v) -> double {
        return (v.size() >= 2 && v.front() > v.back()) ? 1.0 : 0.0;
    });

    fm.registerFunction("lt", [](const std::vector<double>& v) -> double {
        return (v.size() >= 2 && v.front() < v.back()) ? 1.0 : 0.0;
    });

    fm.registerFunction("gte", [](const std::vector<double>& v) -> double {
        return (v.size() >= 2 && v.front() >= v.back()) ? 1.0 : 0.0;
    });

    fm.registerFunction("lte", [](const std::vector<double>& v) -> double {
        return (v.size() >= 2 && v.front() <= v.back()) ? 1.0 : 0.0;
    });

    fm.registerFunction("eq", [](const std::vector<double>& v) -> double {
        return (v.size() >= 2 && std::abs(v.front() - v.back()) < EPSILON) ? 1.0 : 0.0;
    });

    fm.registerFunction("neq", [](const std::vector<double>& v) -> double {
        return (v.size() >= 2 && std::abs(v.front() - v.back()) >= EPSILON) ? 1.0 : 0.0;
    });

    // ──── Logical (treat >0.5 as true) ────

    fm.registerFunction("and", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        for (double x : v) if (x <= 0.5) return 0.0;
        return 1.0;
    });

    fm.registerFunction("or", [](const std::vector<double>& v) -> double {
        for (double x : v) if (x > 0.5) return 1.0;
        return 0.0;
    });

    fm.registerFunction("not", [](const std::vector<double>& v) -> double {
        return (!v.empty() && v.back() <= 0.5) ? 1.0 : 0.0;
    });

    // ──── Financial derivations ────

    fm.registerFunction("zscore", [](const std::vector<double>& v) -> double {
        if (v.size() < 3) return 0.0;
        double n = static_cast<double>(v.size());
        double s = 0.0;
        for (double x : v) s += x;
        double mean = s / n;
        double ss = 0.0;
        for (double x : v) { double d = x - mean; ss += d * d; }
        double sd = std::sqrt(ss / n);
        return std::abs(sd) > EPSILON ? (v.back() - mean) / sd : 0.0;
    });

    fm.registerFunction("ema_weight", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        size_t n = v.size();
        double k = 2.0 / (static_cast<double>(n) + 1.0);
        double ema = v[0];
        for (size_t i = 1; i < n; ++i)
            ema = k * v[i] + (1.0 - k) * ema;
        return ema;
    });

    fm.registerFunction("cumulative_return", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double base = v.front();
        return std::abs(base) > EPSILON ? (v.back() - base) / base : 0.0;
    });
}

} // namespace gma
