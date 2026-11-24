#!/usr/bin/env python3
"""Verify the shipped executable contains no KEEFI financial execution symbols."""
import argparse
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('executable', type=Path)
args = parser.parse_args()
if not args.executable.is_file():
    parser.error('executable must exist; a missing binary cannot pass')
result = subprocess.run(['nm', '-C', str(args.executable)], check=True, capture_output=True, text=True)
forbidden = [
    r'keefi::(?:Trading|BrokerTrading|FuturesTrading)Controller::(?:placeOrder|cancelOrder|amendOrder|cancelAll|testOrder|setLiveEnabled)\(',
    r'keefi::(?:TradingWindow|BrokerTradingWindow|FuturesTradingWindow|SwapWindow|SwapController|WalletBridge|JupiterSwapApi|SolanaSwapPolicy)::',
    r'keefi::SolanaTransactionCodec::(?:decode|validateUnsigned|verifySigned)\(',
]
violations = [line for line in result.stdout.splitlines() if any(re.search(pattern, line) for pattern in forbidden)]
for line in violations[:12]:
    print(line)
# Reject stripped/wrong binaries too: retained read support must be present.
required = ['keefi::TradingController::refreshAssets(', 'keefi::BrokerTradingController::refreshAssets(',
            'keefi::FuturesTradingController::refreshAssets(', 'keefi::WalletReadController::refresh(',
            'keefi::SolanaTransactionCodec::publicKeyValid(']
missing = [name for name in required if name not in result.stdout]
print(f'financial execution symbols={len(violations)}; missing read symbols={len(missing)}')
for name in missing:
    print('missing:', name)
raise SystemExit(bool(violations or missing))
