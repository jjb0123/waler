#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace waler {

// Rolling realized-volatility estimator over mid-price observations: keeps
// a fixed-size window of log-returns and reports their stddev, annualized
// using the *actual* elapsed wall-clock time spanned by the window (not an
// assumed sample rate) -- this is the input a market-making desk feeds to
// Black-Scholes when the only thing being observed is the underlying
// itself, tick by tick, rather than an options-market implied-vol surface.
class RealizedVolEstimator {
public:
  explicit RealizedVolEstimator(std::size_t window) : window_(window) {
    returns_.reserve(window);
  }

  // now_ns must be monotonically non-decreasing across calls (e.g. TSC
  // converted via tsc_to_ns()).
  void observe(double mid_price, double now_ns) {
    if (have_prev_) {
      const double log_return = std::log(mid_price / prev_price_);
      if (returns_.size() >= window_) {
        returns_.erase(returns_.begin());
      }
      returns_.push_back(log_return);
      if (first_ns_ < 0.0) {
        first_ns_ = prev_ns_;
      }
      last_ns_ = now_ns;
    }
    prev_price_ = mid_price;
    prev_ns_ = now_ns;
    have_prev_ = true;
  }

  // Returns 0.0 until the window has at least two returns in it.
  [[nodiscard]] double annualizedVol() const {
    if (returns_.size() < 2) {
      return 0.0;
    }

    double mean = 0.0;
    for (double r : returns_) {
      mean += r;
    }
    mean /= static_cast<double>(returns_.size());

    double var = 0.0;
    for (double r : returns_) {
      const double d = r - mean;
      var += d * d;
    }
    var /= static_cast<double>(returns_.size());
    const double stddev = std::sqrt(var);

    const double span_seconds = (last_ns_ - first_ns_) / 1e9;
    if (span_seconds <= 0.0) {
      return 0.0;
    }
    const double avg_seconds_per_sample = span_seconds / static_cast<double>(returns_.size());
    constexpr double kSecondsPerYear = 365.25 * 24.0 * 3600.0;
    return stddev * std::sqrt(kSecondsPerYear / avg_seconds_per_sample);
  }

private:
  std::size_t window_;
  std::vector<double> returns_;
  bool have_prev_ = false;
  double prev_price_ = 0.0;
  double prev_ns_ = 0.0;
  double first_ns_ = -1.0;
  double last_ns_ = 0.0;
};

}  // namespace waler
