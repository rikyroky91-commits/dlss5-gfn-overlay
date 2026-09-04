#pragma once

#include <string>

#include "config.h"

namespace gfn {

struct RunOptions {
    // Stop after this many enhanced frames and print the timing summary. 0 runs
    // until the user quits.
    int bench_frames = 0;
    // Print a stats line every N seconds. 0 disables it.
    int stats_interval_seconds = 0;
};

// Returns the process exit code.
int Run(const Config& config, const RunOptions& options);

}  // namespace gfn
