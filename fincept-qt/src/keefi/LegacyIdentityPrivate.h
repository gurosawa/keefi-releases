#pragma once

#include <array>

namespace keefi::legacy {

// Frozen pre-transition logical identity inputs. These are deliberately explicit:
// imported origins must retain their IDs and previously exported-record identity.
// They are not storage encryption domains and never select a new container format.
inline constexpr std::array<const char*, 19> IdentityDomains{{
    "FINIX/current-assets/credential-binding/v1",
    "FINIX/event-reference/v1",
    "FINIX/portable-record-scope/v1",
    "FINIX/valuation-preferences/v1",
    "FINIX/BrokerAccountIdentity/v1",
    "FINIX/BrokerConfiguredIdentity/v1",
    "FINIX/KIS/CredentialIdentity/v1",
    "FINIX/KIS/HoldingIdentity/v1",
    "finix.kiwoom.account.v1",
    "finix.kiwoom.credentials.v1",
    "finix.kiwoom.cursor.v1",
    "finix.kiwoom.position.v1",
    "finix.ls.account.v1",
    "finix.ls.credentials.v1",
    "finix.ls.cursor.v1",
    "finix.ls.position.v1",
    "finix.okx.account.v1",
    "finix.solana.mainnet:",
    "finix.hyperliquid.mainnet:"
}};

} // namespace finix::legacy
