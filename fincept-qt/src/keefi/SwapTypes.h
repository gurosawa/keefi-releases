#pragma once
#include "keefi/TradingTypes.h"
#include <QByteArray>
#include <QJsonArray>
#include <optional>
namespace keefi {
inline const QString SolMint=QStringLiteral("So11111111111111111111111111111111111111112");
inline const QString UsdcMint=QStringLiteral("EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v");
enum class SwapOperation { Order,Execute,Genesis,Balance,TokenAccounts,Accounts,BlockHeight,Fee,Simulate,SignatureStatus,Transaction,RentMinimum,TokenMetadata,TokenPrices,BlockTimes };
struct SwapPendingRecord {
    QString id,phantomAccountId,jupiterAccountId,credentialRef,chain,wallet,inputMint,outputMint,inputAtomic,minimumRouteOutput,slippageBps;
    QString requestId,transactionId,messageHash,lastValidBlockHeight,createdAt,state;
    QString confirmedSlot,actualFeeLamports,actualInputAtomic,actualOutputAtomic;
    bool operator==(const SwapPendingRecord&) const = default;
};
struct SwapRequest {
    SwapOperation operation=SwapOperation::Genesis; QByteArray method="POST"; QUrl url; QMap<QByteArray,QByteArray> headers; QByteArray body;
};
struct SwapApiResult {
    bool ok=false,unknown=false,rateLimited=false,authenticationFailed=false;
    QString code,message; QJsonObject data; QJsonValue value;
};
} // namespace keefi
