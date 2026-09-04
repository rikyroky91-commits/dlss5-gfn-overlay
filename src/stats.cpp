#include "stats.h"

#include <cstdio>

namespace gfn {

std::string PipelineStats::Summary() const {
    char buffer[512];
    const double total_p50 = total_ms.Percentile(0.50);
    const double fps = total_p50 > 0.0 ? 1000.0 / total_p50 : 0.0;
    std::snprintf(buffer, sizeof(buffer),
                  "frames %llu | capture %.1f/%.1f ms | neural %.1f/%.1f ms | "
                  "present %.1f/%.1f ms | total %.1f/%.1f ms | %.1f fps | dropped %llu | "
                  "worker restarts %llu",
                  static_cast<unsigned long long>(total_ms.count()), capture_ms.Percentile(0.50),
                  capture_ms.Percentile(0.95), worker_ms.Percentile(0.50),
                  worker_ms.Percentile(0.95), present_ms.Percentile(0.50),
                  present_ms.Percentile(0.95), total_p50, total_ms.Percentile(0.95), fps,
                  static_cast<unsigned long long>(frames_dropped),
                  static_cast<unsigned long long>(worker_restarts));
    return buffer;
}

}  // namespace gfn
