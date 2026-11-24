#pragma once
#include <array>
#include <utility>
namespace keefi::legacy {
// Explicit import aliases. Reviewed user packs retain their original bytes and SHA.
inline constexpr std::array<std::pair<const char*, const char*>, 13> PublicFormats{{
    {"finix-chart-studies/1", "keefi-chart-studies/1"},
    {"finix-chart-studies/2", "keefi-chart-studies/2"},
    {"finix-chart-studies/3", "keefi-chart-studies/3"},
    {"finix-chart-studies/4", "keefi-chart-studies/4"},
    {"finix-chart-drawings/1", "keefi-chart-drawings/1"},
    {"finix-chart-alert-ui/1", "keefi-chart-alert-ui/1"},
    {"finix-chart-observations/1", "keefi-chart-observations/1"},
    {"finix-watchlist-workspace/2", "keefi-watchlist-workspace/2"},
    {"finix-movement-colors/1", "keefi-movement-colors/1"},
    {"finix-stock-sessions/1", "keefi-stock-sessions/1"},
    {"finix-chart-phrases/1", "keefi-chart-phrases/1"},
    {"finix-liquidation-phrase/1", "keefi-liquidation-phrase/1"},
    {"finix-valuation-v2", "keefi-valuation-v2"}
}};
} // namespace finix::legacy
