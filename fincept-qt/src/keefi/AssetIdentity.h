#pragma once
#include "keefi/AssetValuation.h"

namespace keefi {
// Display identity only. Never changes the evaluator's asset or ledger amount.
using AssetKey = QString;
inline constexpr int AssetIdentityRegistryVersion = 1;
AssetKey assetIdentity(const AssetSnapshot&, const AssetComponent&);
// The same registry for an explicitly identified public exchange instrument.
AssetKey publicAssetIdentity(const QString& provider, const QString& asset);
}
