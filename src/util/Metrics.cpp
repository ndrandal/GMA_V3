// src/util/Metrics.cpp (or wherever this lives)
//
// NOTE:
// MetricRegistry is now fully header-only in gma/Metrics.hpp.
// This TU intentionally exists to keep build systems that expect a .cpp happy,
// but it must NOT provide out-of-line definitions (ODR / duplicate symbol risk).

#include "gma/Metrics.hpp"
