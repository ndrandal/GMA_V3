// src/util/Metrics.cpp
//
// NOTE (ENC-788):
// gma::util::MetricRegistry is now fully header-only and has exactly one
// definition, in gma/util/Metrics.hpp. This TU intentionally exists only to
// keep build systems that expect a .cpp happy; it must NOT provide any
// out-of-line definitions (ODR / duplicate-symbol risk).

#include "gma/util/Metrics.hpp"
