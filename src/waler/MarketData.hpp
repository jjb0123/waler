#pragma once

#include <cstddef>
#include <cstdint>

namespace waler::market_data {

inline constexpr std::size_t kSymbolLen = 8;

inline constexpr std::uint8_t kAddOrderType = 'A';
inline constexpr std::uint8_t kCancelOrderType = 'X';
inline constexpr std::uint8_t kExecuteType = 'E';
inline constexpr std::uint8_t kQuoteType = 'Q';

#pragma pack(push, 1)
struct AddOrderMessage {
  std::uint8_t msg_type;
  std::uint64_t timestamp_ns;
  std::uint32_t order_id;
  std::uint8_t side;
  std::uint32_t price;
  std::uint32_t quantity;
  char symbol[kSymbolLen];
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

static_assert(sizeof(AddOrderMessage) == 30);
static_assert(sizeof(CancelOrderMessage) == 17);
static_assert(sizeof(ExecuteMessage) == 21);
static_assert(sizeof(QuoteMessage) == 25);

} // namespace waler::market_data
