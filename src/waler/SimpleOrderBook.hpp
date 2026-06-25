#pragma once

#include <bit>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <waler/MarketData.hpp>
#include <waler/types.h>

namespace waler {

class SimpleOrderBook {
public:
  struct Level {
    PriceCents price = 0;
    std::uint32_t quantity = 0;
  };

  struct Quote {
    std::uint64_t timestamp_ns = 0;
    Level bid{};
    Level ask{};
  };

  void clear() {
    orders_.clear();
    bids_.clear();
    asks_.clear();
    last_quote_.reset();
  }

  [[nodiscard]] std::size_t orderCount() const {
    return orders_.size();
  }

  [[nodiscard]] std::optional<Level> bestBid() const {
    if (bids_.empty()) {
      return std::nullopt;
    }
    const auto& [price, qty] = *bids_.begin();
    return Level{price, narrowQuantity(qty)};
  }

  [[nodiscard]] std::optional<Level> bestAsk() const {
    if (asks_.empty()) {
      return std::nullopt;
    }
    const auto& [price, qty] = *asks_.begin();
    return Level{price, narrowQuantity(qty)};
  }

  [[nodiscard]] std::optional<Quote> lastQuote() const {
    return last_quote_;
  }

  void printAnsi(std::FILE* out = stdout, std::size_t depth = 10) const {
    if (out == nullptr) {
      return;
    }

    static constexpr const char* kReset = "\033[0m";
    static constexpr const char* kBid = "\033[32m";
    static constexpr const char* kAsk = "\033[31m";
    static constexpr const char* kHeader = "\033[1;37m";
    static constexpr const char* kDim = "\033[2m";

    std::vector<Level> ask_levels;
    ask_levels.reserve(depth);
    for (const auto& [price, qty] : asks_) {
      if (ask_levels.size() >= depth) {
        break;
      }
      ask_levels.push_back(Level{price, narrowQuantity(qty)});
    }

    std::vector<Level> bid_levels;
    bid_levels.reserve(depth);
    for (const auto& [price, qty] : bids_) {
      if (bid_levels.size() >= depth) {
        break;
      }
      bid_levels.push_back(Level{price, narrowQuantity(qty)});
    }

    std::fprintf(out, "%s%-25s | %-25s%s\n", kHeader, "BID", "ASK", kReset);
    std::fprintf(out, "%s%10s %14s | %14s %10s%s\n",
                 kDim, "QTY", "PRICE", "PRICE", "QTY", kReset);

    for (std::size_t i = 0; i < depth; ++i) {
      if (i < bid_levels.size()) {
        std::fprintf(out, "%s%10u %14.2f%s",
                     kBid,
                     bid_levels[i].quantity,
                     static_cast<double>(bid_levels[i].price) / 100.0,
                     kReset);
      } else {
        std::fprintf(out, "%25s", "");
      }

      std::fprintf(out, " | ");

      if (i < ask_levels.size()) {
        std::fprintf(out, "%s%14.2f %10u%s",
                     kAsk,
                     static_cast<double>(ask_levels[i].price) / 100.0,
                     ask_levels[i].quantity,
                     kReset);
      } else {
        std::fprintf(out, "%25s", "");
      }

      std::fputc('\n', out);
    }
  }

  void printDepthChart(std::FILE* out = stdout, std::size_t depth = 10, std::size_t width = 24) const {
    if (out == nullptr) {
      return;
    }

    static constexpr const char* kReset = "\033[0m";
    static constexpr const char* kBid = "\033[32m";
    static constexpr const char* kAsk = "\033[31m";
    static constexpr const char* kHeader = "\033[1;37m";
    static constexpr const char* kDim = "\033[2m";

    struct DepthLevel {
      PriceCents price = 0;
      std::uint64_t cumulative_qty = 0;
    };

    const std::size_t chart_height = width == 0 ? 1 : width;
    const int hist_width = static_cast<int>(depth * 2 + 1);
    const int data_width = 45;

    std::vector<DepthLevel> ask_depth;
    ask_depth.reserve(depth);
    std::uint64_t ask_cumulative = 0;
    for (const auto& [price, qty] : asks_) {
      if (ask_depth.size() >= depth) {
        break;
      }
      ask_cumulative += qty;
      ask_depth.push_back(DepthLevel{price, ask_cumulative});
    }

    std::vector<DepthLevel> bid_depth;
    bid_depth.reserve(depth);
    std::uint64_t bid_cumulative = 0;
    for (const auto& [price, qty] : bids_) {
      if (bid_depth.size() >= depth) {
        break;
      }
      bid_cumulative += qty;
      bid_depth.push_back(DepthLevel{price, bid_cumulative});
    }

    std::uint64_t max_cumulative = 0;
    for (const auto& level : bid_depth) {
      max_cumulative = std::max(max_cumulative, level.cumulative_qty);
    }
    for (const auto& level : ask_depth) {
      max_cumulative = std::max(max_cumulative, level.cumulative_qty);
    }
    if (max_cumulative == 0) {
      max_cumulative = 1;
    }

    std::fprintf(out, "%s%-*s | %-*s%s\n",
                 kHeader,
                 hist_width,
                 "MARKET DEPTH",
                 data_width,
                 "RAW DEPTH",
                 kReset);
    std::fprintf(out, "%s%*s | %10s %10s %10s %10s%s\n",
                 kDim,
                 hist_width,
                 "BIDS|ASKS",
                 "BID QTY",
                 "BID PX",
                 "ASK QTY",
                 "ASK PX",
                 kReset);

    std::vector<std::size_t> bid_heights(depth, 0);
    for (std::size_t i = 0; i < bid_depth.size(); ++i) {
      bid_heights[i] = static_cast<std::size_t>((bid_depth[i].cumulative_qty * chart_height) / max_cumulative);
      if (bid_heights[i] == 0 && bid_depth[i].cumulative_qty > 0) {
        bid_heights[i] = 1;
      }
    }

    std::vector<std::size_t> ask_heights(depth, 0);
    for (std::size_t i = 0; i < ask_depth.size(); ++i) {
      ask_heights[i] = static_cast<std::size_t>((ask_depth[i].cumulative_qty * chart_height) / max_cumulative);
      if (ask_heights[i] == 0 && ask_depth[i].cumulative_qty > 0) {
        ask_heights[i] = 1;
      }
    }

    const std::size_t total_rows = std::max(chart_height, depth);
    for (std::size_t row = 0; row < total_rows; ++row) {
      if (row < chart_height) {
        const std::size_t threshold = chart_height - row;
        for (std::size_t col = depth; col > 0; --col) {
          const std::size_t level_index = col - 1;
          if (level_index < bid_depth.size() && bid_heights[level_index] >= threshold) {
            std::fprintf(out, "%s#%s", kBid, kReset);
          } else {
            std::fputc(' ', out);
          }
        }

        std::fprintf(out, "%s|%s", kDim, kReset);

        for (std::size_t level_index = 0; level_index < depth; ++level_index) {
          if (level_index < ask_depth.size() && ask_heights[level_index] >= threshold) {
            std::fprintf(out, "%s#%s", kAsk, kReset);
          } else {
            std::fputc(' ', out);
          }
        }
      } else {
        std::fprintf(out, "%*s", hist_width, "");
      }

      std::fprintf(out, " | ");

      if (row < depth) {
        const bool has_bid = row < bid_depth.size();
        const bool has_ask = row < ask_depth.size();
        if (has_bid) {
          std::fprintf(out, "%10llu %10.2f",
                       static_cast<unsigned long long>(bid_depth[row].cumulative_qty),
                       static_cast<double>(bid_depth[row].price) / 100.0);
        } else {
          std::fprintf(out, "%10s %10s", "", "");
        }

        std::fprintf(out, " ");

        if (has_ask) {
          std::fprintf(out, "%10llu %10.2f",
                       static_cast<unsigned long long>(ask_depth[row].cumulative_qty),
                       static_cast<double>(ask_depth[row].price) / 100.0);
        } else {
          std::fprintf(out, "%10s %10s", "", "");
        }
      }

      std::fputc('\n', out);
    }

    std::fprintf(out, "%s", kDim);
    for (std::size_t i = 0; i < depth; ++i) {
      std::fputc('-', out);
    }
    std::fputc('+', out);
    for (std::size_t i = 0; i < depth; ++i) {
      std::fputc('-', out);
    }
    std::fprintf(out, "%s\n", kReset);
  }

  bool apply(const market_data::AddOrderMessage& message) {
    const auto it = orders_.find(message.order_id);
    if (it != orders_.end()) {
      removeFromLevels(it->second.side, it->second.price, it->second.quantity);
      it->second = RestingOrder{message.side, message.price, message.quantity, message.timestamp_ns};
    } else {
      orders_.emplace(
          message.order_id,
          RestingOrder{message.side, message.price, message.quantity, message.timestamp_ns});
    }
    addToLevels(message.side, message.price, message.quantity);
    return true;
  }

  bool apply(const market_data::CancelOrderMessage& message) {
    const auto it = orders_.find(message.order_id);
    if (it == orders_.end()) {
      return false;
    }

    const std::uint32_t cancel_qty =
        message.quantity == 0 ? it->second.quantity : message.quantity;
    reduceOrder(it, cancel_qty);
    return true;
  }

  bool apply(const market_data::ExecuteMessage& message) {
    const auto it = orders_.find(message.order_id);
    if (it == orders_.end()) {
      return false;
    }

    reduceOrder(it, message.exec_qty);
    return true;
  }

  bool apply(const market_data::QuoteMessage& message) {
    last_quote_ = Quote{
        .timestamp_ns = message.timestamp_ns,
        .bid = Level{message.bid_price, message.bid_qty},
        .ask = Level{message.ask_price, message.ask_qty},
    };
    return true;
  }

  bool applyMessage(std::span<const std::byte> payload) {
    if (payload.size() < sizeof(market_data::MessageHeader)) {
      return false;
    }

    const auto* header = reinterpret_cast<const market_data::MessageHeader*>(payload.data());
    switch (header->msg_type) {
      case market_data::MessageType::AddOrder:
        return applyPayload<market_data::AddOrderMessage>(payload);
      case market_data::MessageType::CancelOrder:
        return applyPayload<market_data::CancelOrderMessage>(payload);
      case market_data::MessageType::Execute:
        return applyPayload<market_data::ExecuteMessage>(payload);
      case market_data::MessageType::Quote:
        return applyPayload<market_data::QuoteMessage>(payload);
    }
    return false;
  }

  bool applyPacket(std::span<const std::byte> packet) {
    if (packet.size() < sizeof(std::uint16_t)) {
      return false;
    }

    std::uint16_t wire_len = 0;
    std::memcpy(&wire_len, packet.data(), sizeof(wire_len));
    const std::size_t payload_size = fromNetwork16(wire_len);
    const std::size_t packet_size = sizeof(wire_len) + payload_size;
    if (packet.size() < packet_size) {
      return false;
    }

    return applyNetworkPayload(packet.subspan(sizeof(wire_len), payload_size));
  }

private:
  struct RestingOrder {
    std::uint8_t side = 0;
    PriceCents price = 0;
    std::uint32_t quantity = 0;
    std::uint64_t timestamp_ns = 0;
  };

  using BidLevels = std::map<PriceCents, std::uint64_t, std::greater<PriceCents>>;
  using AskLevels = std::map<PriceCents, std::uint64_t>;
  using OrderMap = std::unordered_map<std::uint32_t, RestingOrder>;

  OrderMap orders_{};
  BidLevels bids_{};
  AskLevels asks_{};
  std::optional<Quote> last_quote_{};

  template <typename Message>
  bool applyPayload(std::span<const std::byte> payload) {
    if (payload.size() != sizeof(Message)) {
      return false;
    }

    Message message{};
    std::memcpy(&message, payload.data(), sizeof(message));
    return apply(message);
  }

  bool applyNetworkPayload(std::span<const std::byte> payload) {
    if (payload.size() < sizeof(market_data::MessageHeader)) {
      return false;
    }

    const auto* header = reinterpret_cast<const market_data::MessageHeader*>(payload.data());
    switch (header->msg_type) {
      case market_data::MessageType::AddOrder: {
        market_data::AddOrderMessage message{};
        if (!copyMessage(payload, message)) {
          return false;
        }
        message.timestamp_ns = fromNetwork64(message.timestamp_ns);
        message.order_id = fromNetwork32(message.order_id);
        message.price = fromNetwork32(message.price);
        message.quantity = fromNetwork32(message.quantity);
        return apply(message);
      }
      case market_data::MessageType::CancelOrder: {
        market_data::CancelOrderMessage message{};
        if (!copyMessage(payload, message)) {
          return false;
        }
        message.timestamp_ns = fromNetwork64(message.timestamp_ns);
        message.order_id = fromNetwork32(message.order_id);
        message.quantity = fromNetwork32(message.quantity);
        return apply(message);
      }
      case market_data::MessageType::Execute: {
        market_data::ExecuteMessage message{};
        if (!copyMessage(payload, message)) {
          return false;
        }
        message.timestamp_ns = fromNetwork64(message.timestamp_ns);
        message.order_id = fromNetwork32(message.order_id);
        message.exec_qty = fromNetwork32(message.exec_qty);
        message.exec_price = fromNetwork32(message.exec_price);
        return apply(message);
      }
      case market_data::MessageType::Quote: {
        market_data::QuoteMessage message{};
        if (!copyMessage(payload, message)) {
          return false;
        }
        message.timestamp_ns = fromNetwork64(message.timestamp_ns);
        message.bid_price = fromNetwork32(message.bid_price);
        message.bid_qty = fromNetwork32(message.bid_qty);
        message.ask_price = fromNetwork32(message.ask_price);
        message.ask_qty = fromNetwork32(message.ask_qty);
        return apply(message);
      }
    }
    return false;
  }

  template <typename Message>
  static bool copyMessage(std::span<const std::byte> payload, Message& message) {
    if (payload.size() != sizeof(Message)) {
      return false;
    }
    std::memcpy(&message, payload.data(), sizeof(message));
    return true;
  }

  void addToLevels(std::uint8_t side, PriceCents price, std::uint32_t quantity) {
    if (side == 'B') {
      bids_[price] += quantity;
      return;
    }
    if (side == 'S') {
      asks_[price] += quantity;
    }
  }

  void removeFromLevels(std::uint8_t side, PriceCents price, std::uint32_t quantity) {
    if (side == 'B') {
      subtractLevel(bids_, price, quantity);
      return;
    }
    if (side == 'S') {
      subtractLevel(asks_, price, quantity);
    }
  }

  void reduceOrder(OrderMap::iterator it, std::uint32_t reduce_qty) {
    const std::uint32_t removed_qty = reduce_qty >= it->second.quantity ? it->second.quantity : reduce_qty;
    removeFromLevels(it->second.side, it->second.price, removed_qty);
    it->second.quantity -= removed_qty;
    if (it->second.quantity == 0) {
      orders_.erase(it);
    }
  }

  template <typename Levels>
  static void subtractLevel(Levels& levels, PriceCents price, std::uint32_t quantity) {
    const auto it = levels.find(price);
    if (it == levels.end()) {
      return;
    }
    if (it->second <= quantity) {
      levels.erase(it);
      return;
    }
    it->second -= quantity;
  }

  [[nodiscard]] static std::uint32_t narrowQuantity(std::uint64_t quantity) {
    if (quantity > std::numeric_limits<std::uint32_t>::max()) {
      return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(quantity);
  }

  [[nodiscard]] static std::uint16_t fromNetwork16(std::uint16_t value) {
    if constexpr (std::endian::native == std::endian::little) {
      return __builtin_bswap16(value);
    }
    return value;
  }

  [[nodiscard]] static std::uint32_t fromNetwork32(std::uint32_t value) {
    if constexpr (std::endian::native == std::endian::little) {
      return __builtin_bswap32(value);
    }
    return value;
  }

  [[nodiscard]] static std::uint64_t fromNetwork64(std::uint64_t value) {
    if constexpr (std::endian::native == std::endian::little) {
      return __builtin_bswap64(value);
    }
    return value;
  }
};

} // namespace waler
