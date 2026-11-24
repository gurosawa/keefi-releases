#!/usr/bin/env python3
"""Independent Decimal check of a fixed synthetic G8 candle study; no I/O to accounts."""
import argparse
import json
from decimal import Decimal, localcontext
from pathlib import Path


def calculate():
    with localcontext() as context:
        context.prec = 50
        closes = [Decimal(100 + (i * 7) % 13 + i) / 10 for i in range(60)]
        ranges = [max(Decimal(4), abs(closes[i] + 2 - closes[i-1]),
                      abs(closes[i] - 2 - closes[i-1])) for i in range(1, len(closes))]
        mean = sum(closes[-20:]) / 20
        sigma = (sum((value - mean) ** 2 for value in closes[-20:]) / 20).sqrt()
        ema = sum(closes[:20]) / 20
        for value in closes[20:]:
            ema = (value * 2 + ema * 19) / 21
        atr = sum(ranges[:20]) / 20
        for value in ranges[20:]:
            atr = (atr * 19 + value) / 20
        return {"bandLow": mean - 2*sigma, "bandHigh": mean + 2*sigma,
                "channelLow": ema - Decimal("1.5")*atr,
                "channelHigh": ema + Decimal("1.5")*atr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-result", type=Path)
    args = parser.parse_args()
    expected = calculate()
    compressed = (expected["bandLow"] >= expected["channelLow"] and
                  expected["bandHigh"] <= expected["channelHigh"])
    if args.cpp_result:
        cpp = json.loads(args.cpp_result.read_text())
        assert cpp["format"] == "keefi-g8-candle-crosscheck/1"
        assert cpp["available"] and cpp["compressed"] == compressed
        for name, value in expected.items():
            assert abs(Decimal(cpp[name]) - value) < Decimal("1e-10"), name
    print(json.dumps({"synthetic": True, "candles": 60,
                      "cpp_fields_matched": 6 if args.cpp_result else 0,
                      "reference": {name: str(value) for name, value in expected.items()},
                      "compressed": compressed}, sort_keys=True))


if __name__ == "__main__":
    main()
