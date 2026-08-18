#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace waler {

struct BlackScholesInputs {
  double spot = 0.0;                  // S: current underlying price
  double strike = 0.0;                // K
  double time_to_expiry_years = 0.0;  // T
  double risk_free_rate = 0.0;        // r
  double volatility = 0.0;            // sigma, annualized
};

struct BlackScholesResult {
  double call_price = 0.0;
  double delta = 0.0;  // d(call)/d(spot) -- what a market maker would trade
                        // in the underlying to stay delta-neutral.
};

// Standard normal CDF via erfc (exact identity, no series approximation).
[[nodiscard]] inline double normalCdf(double x) {
  return 0.5 * std::erfc(-x / std::numbers::sqrt2);
}

// European call, Black-Scholes-Merton, no dividend yield.
[[nodiscard]] inline BlackScholesResult priceCall(const BlackScholesInputs& in) {
  BlackScholesResult out;

  if (in.time_to_expiry_years <= 0.0 || in.volatility <= 0.0 ||
      in.spot <= 0.0 || in.strike <= 0.0) {
    // Degenerate case (expired / no vol quoted yet): intrinsic value.
    out.call_price = std::max(in.spot - in.strike, 0.0);
    out.delta = in.spot > in.strike ? 1.0 : 0.0;
    return out;
  }

  const double sqrt_t = std::sqrt(in.time_to_expiry_years);
  const double d1 = (std::log(in.spot / in.strike) +
                      (in.risk_free_rate + 0.5 * in.volatility * in.volatility) * in.time_to_expiry_years) /
                     (in.volatility * sqrt_t);
  const double d2 = d1 - in.volatility * sqrt_t;

  const double nd1 = normalCdf(d1);
  const double nd2 = normalCdf(d2);

  out.call_price = in.spot * nd1 -
                    in.strike * std::exp(-in.risk_free_rate * in.time_to_expiry_years) * nd2;
  out.delta = nd1;
  return out;
}

}  // namespace waler
