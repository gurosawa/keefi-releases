#pragma once
namespace keefi {
// KEEFI is a read-only product. No build option enables financial execution.
inline constexpr bool readOnlyBuild() { return true; }
}
