#pragma once
#include <QUrl>
#include <memory>
namespace keefi {
class PublicCandleSocket;
// Fixed public trade endpoints only; separate from C3's candle allowlist.
bool isPublicFlowSocketEndpoint(const QUrl&);
std::unique_ptr<PublicCandleSocket> makeNetworkPublicFlowSocket();
}
