#pragma once

#include <cstddef>
#include <cstdint>

#include <waler/types.h>

namespace waler::market_data {

inline constexpr std::size_t kSymbolLen = 8;

enum class MessageType : std::uint8_t {
  AddOrder = 'A',
  CancelOrder = 'X',
  Execute = 'E',
  Quote = 'Q',
};

#pragma pack(push, 1)
struct MessageHeader {
  MessageType msg_type;
};

struct AddOrderMessage {
  MessageHeader header;
  std::uint64_t timestamp_ns;
  std::uint32_t order_id;
  std::uint8_t side;
  PriceCents price;
  std::uint32_t quantity;
  char symbol[kSymbolLen];
};

struct CancelOrderMessage {
  MessageHeader header;
  std::uint64_t timestamp_ns;
  std::uint32_t order_id;
  std::uint32_t quantity;
};

struct ExecuteMessage {
  MessageHeader header;
  std::uint64_t timestamp_ns;
  std::uint32_t order_id;
  std::uint32_t exec_qty;
  PriceCents exec_price;
};

struct QuoteMessage {
  MessageHeader header;
  std::uint64_t timestamp_ns;
  PriceCents bid_price;
  std::uint32_t bid_qty;
  PriceCents ask_price;
  std::uint32_t ask_qty;
};
#pragma pack(pop)

static_assert(sizeof(AddOrderMessage) == 30);
static_assert(sizeof(CancelOrderMessage) == 17);
static_assert(sizeof(ExecuteMessage) == 21);
static_assert(sizeof(QuoteMessage) == 25);

} // namespace waler::market_data
