#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <string_view>
#include <sys/types.h>

namespace waler {

class PacketGenerator {
public:
  static constexpr std::size_t kMaxBookDepth = 20;
  static constexpr std::uint32_t kTickSize = 100;
  static constexpr std::int32_t kInitialMidPrice = 15000;
  static constexpr std::uint32_t kDefaultRate = 1000;
  static constexpr std::uint32_t kMaxOrderId = 0xffffffffU;
  static constexpr std::size_t kMaxSymbolLen = 8;
  static constexpr std::size_t kRestingPool = 512;
  static constexpr double kLambdaLimit = 0.50;
  static constexpr double kLambdaCancel = 0.30;
  static constexpr double kLambdaMarket = 0.20;

#pragma pack(push, 1)
  struct AddOrderMessage {
    std::uint8_t msg_type;
    std::uint64_t timestamp_ns;
    std::uint32_t order_id;
    std::uint8_t side;
    std::uint32_t price;
    std::uint32_t quantity;
    char symbol[kMaxSymbolLen];
  };

  struct CancelOrderMessage {
    std::uint8_t msg_type;
    std::uint64_t timestamp_ns;
    std::uint32_t order_id;
    std::uint32_t quantity;
  };

  struct ExecuteMessage {
    std::uint8_t msg_type;
    std::uint64_t timestamp_ns;
    std::uint32_t order_id;
    std::uint32_t exec_qty;
    std::uint32_t exec_price;
  };

  struct QuoteMessage {
    std::uint8_t msg_type;
    std::uint64_t timestamp_ns;
    std::uint32_t bid_price;
    std::uint32_t bid_qty;
    std::uint32_t ask_price;
    std::uint32_t ask_qty;
  };
#pragma pack(pop)

  struct Config {
    std::uint32_t rate = kDefaultRate;
    std::string_view symbol = "FAKE";
    std::uint64_t seed = std::random_device{}();
  };

  PacketGenerator() : PacketGenerator(Config{}) {}

  explicit PacketGenerator(const Config& config)
      : rng_(config.seed), uniform01_(0.0, 1.0), side_dist_(0, 1), event_dist_(0.0, 1.0) {
    setSymbol(config.symbol);
  }

  void setSymbol(std::string_view symbol) {
    symbol_.fill(' ');
    const auto copy_len = std::min(symbol.size(), symbol_.size());
    std::memcpy(symbol_.data(), symbol.data(), copy_len);
  }

  [[nodiscard]] ssize_t nextPacket(std::uint8_t* dst, std::size_t capacity) {
    if (dst == nullptr) {
      return -1;
    }

    if ((messages_generated_ % 50U) == 0U) {
      evolveMid();
    }

    for (int attempts = 0; attempts < 4; ++attempts) {
      const int event = drawEvent();
      ssize_t len = -1;
      switch (event) {
        case 0:
          len = buildAddOrder(dst, capacity);
          break;
        case 1:
          len = buildCancelOrder(dst, capacity);
          break;
        case 2:
          len = buildExecute(dst, capacity);
          break;
        case 3:
          len = buildQuote(dst, capacity);
          break;
      }

      if (len > 0) {
        ++messages_generated_;
        return len;
      }
    }

    return buildQuote(dst, capacity);
  }

private:
  struct RestingOrder {
    std::uint32_t order_id = 0;
    std::uint32_t price = 0;
    std::uint32_t quantity = 0;
    std::uint8_t side = 0;
  };

  std::array<RestingOrder, kRestingPool> resting_{};
  std::size_t resting_count_ = 0;
  std::int32_t mid_price_ = kInitialMidPrice;
  std::uint32_t next_order_id_ = 1;
  std::array<char, kMaxSymbolLen> symbol_{'F', 'A', 'K', 'E', ' ', ' ', ' ', ' '};
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> uniform01_;
  std::uniform_int_distribution<int> side_dist_;
  std::uniform_real_distribution<double> event_dist_;
  std::uint64_t messages_generated_ = 0;

  [[nodiscard]] static std::uint64_t nowNs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  }

  [[nodiscard]] double expRand(double lambda) {
    double u = 0.0;
    do {
      u = uniform01_(rng_);
    } while (u == 0.0);
    return -std::log(u) / lambda;
  }

  [[nodiscard]] double normRand() {
    double u1 = 0.0;
    do {
      u1 = uniform01_(rng_);
    } while (u1 == 0.0);
    const double u2 = uniform01_(rng_);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi_v<double> * u2);
  }

  [[nodiscard]] std::uint32_t randQty() {
    int q = static_cast<int>(std::exp(std::log(100.0) + 0.8 * normRand()) + 0.5);
    q = std::max(q, 1);
    q = std::min(q, 9900);
    return static_cast<std::uint32_t>(q);
  }

  [[nodiscard]] std::uint32_t randPrice(std::uint8_t side) {
    int dist = static_cast<int>(expRand(0.5) * static_cast<double>(kTickSize));
    dist = std::max(dist, static_cast<int>(kTickSize));
    dist = std::min(dist, static_cast<int>(kMaxBookDepth * kTickSize));
    if (side == 'B') {
      return static_cast<std::uint32_t>(mid_price_ - dist);
    }
    return static_cast<std::uint32_t>(mid_price_ + dist);
  }

  void evolveMid() {
    constexpr double vol = 0.0002;
    const double drift = 0.0;
    mid_price_ = static_cast<std::int32_t>(mid_price_ * std::exp(drift + vol * normRand()));
    if (mid_price_ < static_cast<std::int32_t>(kTickSize)) {
      mid_price_ = static_cast<std::int32_t>(kTickSize);
    }
  }

  [[nodiscard]] static std::uint16_t toNetwork16(std::uint16_t value) {
    if constexpr (std::endian::native == std::endian::little) {
      return __builtin_bswap16(value);
    }
    return value;
  }

  [[nodiscard]] static std::uint32_t toNetwork32(std::uint32_t value) {
    if constexpr (std::endian::native == std::endian::little) {
      return __builtin_bswap32(value);
    }
    return value;
  }

  [[nodiscard]] static std::uint64_t toNetwork64(std::uint64_t value) {
    if constexpr (std::endian::native == std::endian::little) {
      return __builtin_bswap64(value);
    }
    return value;
  }

  template <typename Message>
  [[nodiscard]] ssize_t writeMessage(std::uint8_t* dst, std::size_t capacity, const Message& message) {
    constexpr std::size_t payload_size = sizeof(Message);
    constexpr std::size_t packet_size = sizeof(std::uint16_t) + payload_size;
    if (capacity < packet_size) {
      return -1;
    }

    const std::uint16_t wire_len = toNetwork16(static_cast<std::uint16_t>(payload_size));
    std::memcpy(dst, &wire_len, sizeof(wire_len));
    std::memcpy(dst + sizeof(wire_len), &message, payload_size);
    return static_cast<ssize_t>(packet_size);
  }

  [[nodiscard]] int drawEvent() {
    constexpr double quote_weight = 0.1;
    const double total = kLambdaLimit + kLambdaCancel + kLambdaMarket + quote_weight;
    const double r = event_dist_(rng_) * total;
    if (r < kLambdaLimit) {
      return 0;
    }
    if (r < (kLambdaLimit + kLambdaCancel)) {
      return 1;
    }
    if (r < (kLambdaLimit + kLambdaCancel + kLambdaMarket)) {
      return 2;
    }
    return 3;
  }

  [[nodiscard]] ssize_t buildAddOrder(std::uint8_t* dst, std::size_t capacity) {
    if (resting_count_ >= resting_.size()) {
      return -1;
    }

    const std::uint8_t side = side_dist_(rng_) != 0 ? 'B' : 'S';
    const std::uint32_t price = randPrice(side);
    const std::uint32_t qty = randQty();
    std::uint32_t oid = next_order_id_++;
    if (next_order_id_ == 0) {
      next_order_id_ = 1;
    }

    resting_[resting_count_++] = RestingOrder{oid, price, qty, side};

    AddOrderMessage message{};
    message.msg_type = 'A';
    message.timestamp_ns = toNetwork64(nowNs());
    message.order_id = toNetwork32(oid);
    message.side = side;
    message.price = toNetwork32(price);
    message.quantity = toNetwork32(qty);
    std::memcpy(message.symbol, symbol_.data(), symbol_.size());
    return writeMessage(dst, capacity, message);
  }

  [[nodiscard]] ssize_t buildCancelOrder(std::uint8_t* dst, std::size_t capacity) {
    if (resting_count_ == 0) {
      return -1;
    }

    std::uniform_int_distribution<std::size_t> index_dist(0, resting_count_ - 1);
    const std::size_t idx = index_dist(rng_);
    const std::uint32_t oid = resting_[idx].order_id;
    resting_[idx] = resting_[resting_count_ - 1];
    --resting_count_;

    CancelOrderMessage message{};
    message.msg_type = 'X';
    message.timestamp_ns = toNetwork64(nowNs());
    message.order_id = toNetwork32(oid);
    message.quantity = 0;
    return writeMessage(dst, capacity, message);
  }

  [[nodiscard]] ssize_t buildExecute(std::uint8_t* dst, std::size_t capacity) {
    const std::uint8_t side = side_dist_(rng_) != 0 ? 'B' : 'S';
    std::uint32_t best_price = side == 'B' ? 0U : std::numeric_limits<std::uint32_t>::max();
    ssize_t best_idx = -1;

    for (std::size_t i = 0; i < resting_count_; ++i) {
      if (resting_[i].side != side) {
        continue;
      }

      if ((side == 'B' && resting_[i].price > best_price) ||
          (side == 'S' && resting_[i].price < best_price)) {
        best_price = resting_[i].price;
        best_idx = static_cast<ssize_t>(i);
      }
    }

    if (best_idx < 0) {
      return -1;
    }

    auto& order = resting_[static_cast<std::size_t>(best_idx)];
    const std::uint32_t order_id = order.order_id;
    std::uint32_t exec_qty = randQty();
    if (exec_qty >= order.quantity) {
      exec_qty = order.quantity;
      order = resting_[resting_count_ - 1];
      --resting_count_;
    } else {
      order.quantity -= exec_qty;
    }

    ExecuteMessage message{};
    message.msg_type = 'E';
    message.timestamp_ns = toNetwork64(nowNs());
    message.order_id = toNetwork32(order_id);
    message.exec_qty = toNetwork32(exec_qty);
    message.exec_price = toNetwork32(best_price);
    return writeMessage(dst, capacity, message);
  }

  [[nodiscard]] ssize_t buildQuote(std::uint8_t* dst, std::size_t capacity) {
    std::uint32_t best_bid = 0;
    std::uint32_t best_ask = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t bid_qty = 0;
    std::uint32_t ask_qty = 0;

    for (std::size_t i = 0; i < resting_count_; ++i) {
      const auto& order = resting_[i];
      if (order.side == 'B' && order.price > best_bid) {
        best_bid = order.price;
        bid_qty = order.quantity;
      }
      if (order.side == 'S' && order.price < best_ask) {
        best_ask = order.price;
        ask_qty = order.quantity;
      }
    }

    if (best_bid == 0) {
      best_bid = static_cast<std::uint32_t>(mid_price_ - static_cast<std::int32_t>(kTickSize));
    }
    if (best_ask == std::numeric_limits<std::uint32_t>::max()) {
      best_ask = static_cast<std::uint32_t>(mid_price_ + static_cast<std::int32_t>(kTickSize));
    }

    QuoteMessage message{};
    message.msg_type = 'Q';
    message.timestamp_ns = toNetwork64(nowNs());
    message.bid_price = toNetwork32(best_bid);
    message.bid_qty = toNetwork32(bid_qty);
    message.ask_price = toNetwork32(best_ask);
    message.ask_qty = toNetwork32(ask_qty);
    return writeMessage(dst, capacity, message);
  }
};

static_assert(sizeof(PacketGenerator::AddOrderMessage) == 30);
static_assert(sizeof(PacketGenerator::CancelOrderMessage) == 17);
static_assert(sizeof(PacketGenerator::ExecuteMessage) == 21);
static_assert(sizeof(PacketGenerator::QuoteMessage) == 25);

}  // namespace waler
