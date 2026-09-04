#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace gfn {

// Keeps a bounded window of samples so percentiles stay meaningful during a
// session without growing without bound.
class Samples {
public:
    explicit Samples(size_t capacity = 4096) : capacity_(capacity) {
        values_.reserve(capacity);
    }

    void Add(double value) {
        if (values_.size() < capacity_) {
            values_.push_back(value);
        } else {
            values_[cursor_] = value;
        }
        cursor_ = (cursor_ + 1) % capacity_;
        ++count_;
    }

    bool empty() const { return values_.empty(); }
    uint64_t count() const { return count_; }

    double Percentile(double fraction) const {
        if (values_.empty()) return 0.0;
        std::vector<double> sorted(values_);
        std::sort(sorted.begin(), sorted.end());
        const size_t index = static_cast<size_t>(
            std::clamp(fraction, 0.0, 1.0) * static_cast<double>(sorted.size() - 1) + 0.5);
        return sorted[std::min(index, sorted.size() - 1)];
    }

    double Mean() const {
        if (values_.empty()) return 0.0;
        double total = 0.0;
        for (double value : values_) total += value;
        return total / static_cast<double>(values_.size());
    }

    void Clear() {
        values_.clear();
        cursor_ = 0;
        count_ = 0;
    }

private:
    std::vector<double> values_;
    size_t capacity_;
    size_t cursor_ = 0;
    uint64_t count_ = 0;
};

// Per-stage timings for one pass of the pipeline, in milliseconds.
struct PipelineStats {
    Samples capture_ms;   // GPU convert plus readback to system memory
    Samples worker_ms;    // full round trip through the DLSS worker
    Samples present_ms;   // upload plus present
    Samples total_ms;     // capture through present
    uint64_t frames_dropped = 0;
    uint64_t worker_restarts = 0;

    std::string Summary() const;
    void Clear() {
        capture_ms.Clear();
        worker_ms.Clear();
        present_ms.Clear();
        total_ms.Clear();
        frames_dropped = 0;
    }
};

}  // namespace gfn
