#pragma once
#include <acp/common.hpp>

#include <chrono>
#include <cmath>

namespace acp {
// Streaming timings have bounded storage even in an indefinitely running session.
// Quantiles are histogram upper bounds, within 3.125% of the represented duration.
class TimingSummary {
  public:
    void add(std::chrono::nanoseconds elapsed) {
        auto value = static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed.count(), 0));
        auto positive = std::max<std::uint64_t>(value, 1);
        auto exponent = std::bit_width(positive) - 1;
        auto remainder = positive - (std::uint64_t{1} << exponent);
        auto fraction = exponent >= 5 ? remainder >> (exponent - 5) : remainder << (5 - exponent);
        ++buckets_[exponent * 32 + fraction];
        ++count_;
        total_ns_ += static_cast<double>(value);
        maximum_ns_ = std::max(maximum_ns_, value);
    }
    Json report() const {
        if (!count_)
            return {{"samples", 0}};
        auto percentile = [&](double fraction) {
            auto rank =
                std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(count_ * fraction)));
            std::uint64_t cumulative = 0;
            for (size_t i = 0; i < buckets_.size(); ++i) {
                cumulative += buckets_[i];
                if (cumulative >= rank) {
                    auto upper = std::ldexp(1.0, static_cast<int>(i / 32)) * (1.0 + (i % 32 + 1) / 32.0);
                    return std::min(upper, static_cast<double>(maximum_ns_)) / 1e6;
                }
            }
            return maximum_ns_ / 1e6;
        };
        return {{"samples", count_},
                {"mean", total_ns_ / count_ / 1e6},
                {"p50", percentile(0.5)},
                {"p95", percentile(0.95)},
                {"p99", percentile(0.99)},
                {"max", maximum_ns_ / 1e6},
                {"total", total_ns_ / 1e6},
                {"percentile_method", "histogram-upper-bound"},
                {"max_relative_bucket_width", 1.0 / 32}};
    }

  private:
    std::array<std::uint64_t, 64 * 32> buckets_{};
    std::uint64_t count_{}, maximum_ns_{};
    double total_ns_{};
};

enum class FrameStage {
    Events,
    Pacing,
    NativeDraw,
    Presentation,
    Swap,
    Capture,
    CallbackDrain,
    Startup,
    Count
};
class FrameTimings {
  public:
    bool enabled{};
    template <class F> decltype(auto) measure(FrameStage stage, F &&work) {
        if (!enabled)
            return std::forward<F>(work)();
        struct Scope {
            TimingSummary &summary;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            ~Scope() {
                summary.add(std::chrono::steady_clock::now() - start);
            }
        } scope{summary(stage)};
        return std::forward<F>(work)();
    }
    void record(FrameStage stage, std::chrono::nanoseconds elapsed) {
        if (enabled)
            summary(stage).add(elapsed);
    }
    Json report() const {
        Json stages = Json::object();
        if (!enabled)
            return stages;
        constexpr std::array names{"events", "pacing",  "native_draw",    "presentation",
                                   "swap",   "capture", "callback_drain", "startup"};
        for (size_t i = 0; i < names.size(); ++i)
            stages[names[i]] = stages_ ? (*stages_)[i].report() : Json{{"samples", 0}};
        return stages;
    }

  private:
    std::unique_ptr<std::array<TimingSummary, static_cast<size_t>(FrameStage::Count)>> stages_;
    TimingSummary &summary(FrameStage stage) {
        if (!stages_)
            stages_ = std::make_unique<std::array<TimingSummary, static_cast<size_t>(FrameStage::Count)>>();
        return (*stages_)[static_cast<size_t>(stage)];
    }
};
} // namespace acp
