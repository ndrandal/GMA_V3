#pragma once
//
// ENC-788: `gma::util::MetricRegistry` must have exactly ONE definition, and it
// lives in gma/util/Metrics.hpp. This header used to declare a *second*,
// incompatible class of the same fully-qualified name (different members + an
// out-of-line API that was never defined). Two definitions of the same type in
// one program is an ODR violation (UB). This header now forwards to the single
// canonical definition so only one MetricRegistry ever exists.
//
#include "gma/util/Metrics.hpp"

// Legacy macro spellings kept as thin aliases over the unified API so any
// straggler using them keeps compiling. The live spellings are GMA_METRIC_*
// (defined in gma/util/Metrics.hpp); these METRIC_* names are currently unused
// in-tree but are retained to avoid breaking out-of-tree callers.
#ifndef METRIC_INC
#define METRIC_INC(name, d) ::gma::util::MetricRegistry::instance().increment((name), (d))
#endif
#ifndef METRIC_HIT
#define METRIC_HIT(name)    ::gma::util::MetricRegistry::instance().increment((name), 1.0)
#endif
#ifndef METRIC_SET
#define METRIC_SET(name, v) ::gma::util::MetricRegistry::instance().setGauge((name), (v))
#endif
