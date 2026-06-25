#pragma once


namespace waler {
  // ---- An interface to the network for sending and receiving messages ---
  // This is a high frequency trading application, so the interface should be as
  // low overhead as possible.  As such, the network interface must provide
  // buffers, and the application will fill those buffers and send them.  it
  // also must receive and poll on the network for incoming messages, which gets
  // complicated. We also don't want to have virtual function dispatch here, as
  // it adds overhead and uncontrolled indirection.
  class NetworkInterface {
    // -- TODO --
  };
}