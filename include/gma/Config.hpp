#pragma once

#include <cstddef>

namespace gma {
struct Config {
    static constexpr size_t ListenerQueueMax = 1000;
    static constexpr size_t HistoryMaxSize = 1000;
    static constexpr size_t ThreadPoolSize = 4;
};
}
