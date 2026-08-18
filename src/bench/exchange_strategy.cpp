// Two-thread rig: an "exchange" thread blasts simulated market data over
// UDP via kernel-bypass ef_vi TX, and a "strategy" thread picks packets off
// the NIC via ef_vi RX, applies them to a real order book, reprices a
// synthetic at-the-money call via Black-Scholes off the book's mid, and
// -- when the theoretical price has drifted enough -- sends a two-sided
// quote back out, also via ef_vi TX. Everything runs serially on the
// strategy thread (single-writer: one thread owns the book, no locking).
//
// The egress quote currently goes out into the void -- the exchange port
// isn't listening for it yet, so the loop isn't fully closed. What's timed
// is receive -> parse+apply -> price+decide -> send.
//
// Requires the two Solarflare X4 ports to be cabled together (loopback).

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <waler/BlackScholes.hpp>
#include <waler/EfviInterface.hpp>
#include <waler/EfviUdpSender.hpp>
#include <waler/MarketData.hpp>
#include <waler/PacketGenerator.hpp>
#include <waler/RealizedVol.hpp>
#include <waler/SimpleOrderBook.hpp>
#include <waler/util.hpp>

#include "timing.h"
#include "stats.h"

namespace {

constexpr const char* kExchangeIfname = "enp1s0f0np0";
constexpr const char* kStrategyIfname = "enp1s0f1np1";
constexpr const char* kExchangeIp     = "10.10.10.1";
constexpr const char* kStrategyIp     = "10.10.10.2";
constexpr std::uint16_t kUdpPort      = 30303;
constexpr std::size_t   kDefaultCount = 200000;

// Run this many send/receive cycles before recording anything, so cold
// icache/dcache, an untrained branch predictor, and pre-turbo CPU clocks
// don't pollute the measured percentiles (same idea as WARMUP_ITERS in
// x4_loopback_latency.c).
constexpr std::size_t kWarmupIters = 2000;

// We crafted these frames ourselves (see EfviUdpSender), no IP options.
constexpr std::size_t kEthIpUdpHeaderLen = 14 + 20 + 8;

// -- Black-Scholes inputs. This is a synthetic single option on the
// synthetic underlying PacketGenerator drives, not a real listed contract. --
constexpr double kStrikeDollars      = 150.00;         // ATM at PacketGenerator's initial mid (15000 cents)
constexpr double kInitialExpiryYears = 30.0 / 365.0;    // decays with wall-clock elapsed time
constexpr double kRiskFreeRate       = 0.05;
constexpr double kFallbackVol        = 0.20;            // used until the realized-vol window fills
constexpr std::size_t kVolWindow     = 64;
constexpr double kSecondsPerYear     = 365.25 * 24.0 * 3600.0;

// -- Quoting rule: recompute theo on every book update; only send when it's
// moved enough to matter (avoids quote-flicker/message spam). --
constexpr double kRequoteThreshold = 0.01;   // re-quote once theo moves >= 1 cent
constexpr double kHalfSpread       = 0.02;   // quote 2 cents wide on each side
constexpr std::uint32_t kQuoteQty  = 100;

std::atomic<bool> g_running{true};

void onSigint(int) {
  g_running.store(false, std::memory_order_relaxed);
}

[[nodiscard]] std::uint16_t toNetwork16(std::uint16_t v) {
  if constexpr (std::endian::native == std::endian::little) return __builtin_bswap16(v);
  return v;
}
[[nodiscard]] std::uint32_t toNetwork32(std::uint32_t v) {
  if constexpr (std::endian::native == std::endian::little) return __builtin_bswap32(v);
  return v;
}
[[nodiscard]] std::uint64_t toNetwork64(std::uint64_t v) {
  if constexpr (std::endian::native == std::endian::little) return __builtin_bswap64(v);
  return v;
}

[[nodiscard]] waler::PriceCents dollarsToCents(double dollars) {
  return static_cast<waler::PriceCents>(std::lround(dollars * 100.0));
}

// Wire-encodes a QuoteMessage exactly as PacketGenerator does (2-byte
// length prefix + network-order fields). Returns the encoded length.
[[nodiscard]] std::size_t encodeQuote(std::byte* dst, double bid_dollars, double ask_dollars, std::uint32_t qty) {
  waler::market_data::QuoteMessage msg{};
  msg.header.msg_type = waler::market_data::MessageType::Quote;
  msg.timestamp_ns = toNetwork64(static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
  msg.bid_price = toNetwork32(dollarsToCents(bid_dollars));
  msg.bid_qty   = toNetwork32(qty);
  msg.ask_price = toNetwork32(dollarsToCents(ask_dollars));
  msg.ask_qty   = toNetwork32(qty);

  const std::uint16_t wire_len = toNetwork16(static_cast<std::uint16_t>(sizeof(msg)));
  std::memcpy(dst, &wire_len, sizeof(wire_len));
  std::memcpy(dst + sizeof(wire_len), &msg, sizeof(msg));
  return sizeof(wire_len) + sizeof(msg);
}

void exchangeThreadMain(std::size_t count) {
  const std::size_t total = count + kWarmupIters;

  const auto dst_mac = waler::getInterfaceMac(kStrategyIfname);
  waler::EfviUdpSender sender(kExchangeIfname, kExchangeIp, kUdpPort,
                              kStrategyIp, kUdpPort, dst_mac);

  waler::PacketGenerator gen;
  std::byte buffer[1024];

  std::size_t sent = 0;
  while (g_running.load(std::memory_order_relaxed) && sent < total) {
    const ssize_t bytes = gen.nextPacket(reinterpret_cast<std::uint8_t*>(buffer), sizeof(buffer));
    if (bytes <= 0) {
      continue;
    }
    if (sender.send(buffer, static_cast<std::size_t>(bytes)) == 0) {
      ++sent;
    }
    sender.reapCompletions();
  }

  std::fprintf(stderr, "exchange: sent %zu packets\n", sent);
}

struct Stats {
  stats_t nic_gap;           // inter-arrival gap, NIC hw timestamp domain
  stats_t tsc_gap;           // inter-arrival gap, host TSC-after-poll domain
  stats_t book_apply;        // parse + SimpleOrderBook::apply()
  stats_t pricing_decision;  // vol update + Black-Scholes + requote decision
  stats_t send;              // egress send() itself (only when we requote)
};

void strategyThreadMain(std::size_t count, Stats* stats, std::size_t* received, std::size_t* requoted) {
  const std::size_t total = count + kWarmupIters;

  waler::EfviInterface rx(kStrategyIfname, kStrategyIp, kUdpPort);

  const auto exchange_mac = waler::getInterfaceMac(kExchangeIfname);
  waler::EfviUdpSender egress(kStrategyIfname, kStrategyIp, kUdpPort,
                              kExchangeIp, kUdpPort, exchange_mac);

  waler::SimpleOrderBook book;
  waler::RealizedVolEstimator vol(kVolWindow);
  const double t0_ns = tsc_to_ns(tsc_read());

  double last_quoted_theo = 0.0;
  bool have_quote = false;
  std::size_t n_requoted = 0;

  bool have_prev = false;
  double prev_nic_ns = 0.0;
  std::uint64_t prev_tsc = 0;

  std::size_t n = 0;
  while (g_running.load(std::memory_order_relaxed) && n < total) {
    rx.poll([&](const waler::EfviFrame& frame) {
      if (n >= total) {
        return;
      }
      ++n;

      // First kWarmupIters frames still update prev_tsc/prev_nic_ns (so the
      // first *recorded* gap, right at the warmup/measured boundary, is a
      // real delta) but don't get their gap recorded.
      const bool recording = n > kWarmupIters;

      if (have_prev && recording) {
        stats_record(&stats->tsc_gap, frame.tsc - prev_tsc);
      }
      prev_tsc = frame.tsc;

      if (frame.nic_ts_valid) {
        const double nic_ns = static_cast<double>(frame.nic_ts.tv_sec) * 1e9 +
                               static_cast<double>(frame.nic_ts.tv_nsec) +
                               static_cast<double>(frame.nic_ts.tv_nsec_frac) / 65536.0;
        if (have_prev && recording) {
          // NIC hardware timestamps only, both from this same PHC: exact,
          // no TSC<->PHC correlation needed for a same-domain delta.
          stats_record(&stats->nic_gap, ns_to_tsc(nic_ns - prev_nic_ns));
        }
        prev_nic_ns = nic_ns;
      }

      have_prev = true;

      // -- parse + apply to the order book, timed --
      if (frame.len <= kEthIpUdpHeaderLen) {
        return;
      }
      const auto* payload_bytes = reinterpret_cast<const std::byte*>(frame.data + kEthIpUdpHeaderLen);
      const std::size_t payload_len = frame.len - kEthIpUdpHeaderLen;

      const std::uint64_t t_book_start = tsc_read();
      const bool applied = book.applyPacket({payload_bytes, payload_len});
      const std::uint64_t t_book_done = tsc_read();
      if (recording) {
        stats_record(&stats->book_apply, t_book_done - t_book_start);
      }
      if (!applied) {
        return;
      }

      // -- reprice + decide whether to requote, timed --
      const auto bid = book.bestBid();
      const auto ask = book.bestAsk();
      if (!bid || !ask) {
        return;
      }
      const double mid_dollars =
          (static_cast<double>(bid->price) + static_cast<double>(ask->price)) / 2.0 / 100.0;

      const double now_ns = tsc_to_ns(t_book_done);
      vol.observe(mid_dollars, now_ns);

      const double elapsed_years = (now_ns - t0_ns) / 1e9 / kSecondsPerYear;
      const double time_to_expiry = std::max(kInitialExpiryYears - elapsed_years, 1e-6);
      const double sigma = vol.annualizedVol() > 0.0 ? vol.annualizedVol() : kFallbackVol;

      const waler::BlackScholesInputs bs_in{
          .spot = mid_dollars,
          .strike = kStrikeDollars,
          .time_to_expiry_years = time_to_expiry,
          .risk_free_rate = kRiskFreeRate,
          .volatility = sigma,
      };
      const waler::BlackScholesResult bs_out = waler::priceCall(bs_in);

      const std::uint64_t t_decision_done = tsc_read();
      if (recording) {
        stats_record(&stats->pricing_decision, t_decision_done - t_book_done);
      }

      const bool should_requote =
          !have_quote || std::abs(bs_out.call_price - last_quoted_theo) >= kRequoteThreshold;
      if (should_requote) {
        std::byte out_buf[64];
        const std::size_t out_len = encodeQuote(
            out_buf, bs_out.call_price - kHalfSpread, bs_out.call_price + kHalfSpread, kQuoteQty);

        const std::uint64_t t_send_start = tsc_read();
        egress.send(out_buf, out_len);
        const std::uint64_t t_send_done = tsc_read();
        if (recording) {
          stats_record(&stats->send, t_send_done - t_send_start);
          ++n_requoted;
        }

        last_quoted_theo = bs_out.call_price;
        have_quote = true;
      }
      egress.reapCompletions();
    });
  }

  *received = n;
  *requoted = n_requoted;
  std::fprintf(stderr, "strategy: received %zu packets (%zu warmup, %zu discards), requoted %zu times\n",
               n, kWarmupIters, rx.discard_count(), n_requoted);
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t count = kDefaultCount;
  if (argc > 1) {
    count = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  }

  std::signal(SIGINT, onSigint);

  tsc_calibrate();

  Stats stats{};
  stats_init(&stats.nic_gap, count);
  stats_init(&stats.tsc_gap, count);
  stats_init(&stats.book_apply, count);
  stats_init(&stats.pricing_decision, count);
  stats_init(&stats.send, count);
  std::size_t received = 0;
  std::size_t requoted = 0;

  std::thread strategy(strategyThreadMain, count, &stats, &received, &requoted);
  // Give the RX filter a moment to be installed before the exchange thread
  // starts blasting.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::thread exchange(exchangeThreadMain, count);

  exchange.join();
  g_running.store(false, std::memory_order_relaxed);
  strategy.join();

  stats_result_t nic_result{};
  stats_result_t tsc_result{};
  stats_result_t book_result{};
  stats_result_t decision_result{};
  stats_result_t send_result{};
  stats_compute(&stats.nic_gap, &nic_result);
  stats_compute(&stats.tsc_gap, &tsc_result);
  stats_compute(&stats.book_apply, &book_result);
  stats_compute(&stats.pricing_decision, &decision_result);
  stats_compute(&stats.send, &send_result);

  std::fprintf(stdout, "\nreceived: %zu / %zu (%zu warmup + %zu measured), requoted %zu times\n\n",
               received, count + kWarmupIters, kWarmupIters, count, requoted);
  stats_print(&nic_result, stdout, "inter-arrival gap, NIC hw RX timestamp domain (exact)");
  stats_print(&tsc_result, stdout, "inter-arrival gap, host TSC-after-poll domain (incl. scheduling)");
  stats_print(&book_result, stdout, "parse + order book apply");
  stats_print(&decision_result, stdout, "realized-vol update + Black-Scholes + requote decision");
  stats_print(&send_result, stdout, "egress send() (requotes only)");

  stats_free(&stats.nic_gap);
  stats_free(&stats.tsc_gap);
  stats_free(&stats.book_apply);
  stats_free(&stats.pricing_decision);
  stats_free(&stats.send);
  return 0;
}
