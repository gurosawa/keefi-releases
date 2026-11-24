#pragma once
#include <memory>
namespace keefi {
class PublicCandleSocket;
// Constructed only by the application entrypoint. The model's default path
// never opens a real public WebSocket in tests or standalone tools.
std::unique_ptr<PublicCandleSocket> makeNetworkPublicCandleSocket();
}
