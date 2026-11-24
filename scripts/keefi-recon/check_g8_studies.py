#!/usr/bin/env python3
"""Independent Decimal arithmetic for the fixed public G8 study fixture."""
import json
import sys
from decimal import Decimal, localcontext


def expected():
    with localcontext() as context:
        context.prec = 50
        d = Decimal
        close = [d(100 + (i * 7) % 13 + i) / 10 for i in range(60)]
        high, low = [c + 2 for c in close], [c - 2 for c in close]
        volume = [d(2 + (i * 17) % 7) for i in range(60)]
        tr = [max(high[i] - low[i], abs(high[i] - close[i-1]), abs(low[i] - close[i-1])) for i in range(1, 60)]
        plus, minus = [], []
        for i in range(1, 60):
            up, down = high[i] - high[i-1], low[i-1] - low[i]
            plus.append(max(up, d(0)) if up > down else d(0))
            minus.append(max(down, d(0)) if down > up else d(0))

        def wilder_sum(series):
            values = [sum(series[:14])]
            for value in series[14:]:
                values.append(values[-1] * d(13) / 14 + value)
            return values

        trs, pos, neg = map(wilder_sum, (tr, plus, minus))
        pdi = [p / t * 100 if t else d(0) for p, t in zip(pos, trs)]
        mdi = [m / t * 100 if t else d(0) for m, t in zip(neg, trs)]
        dx = [abs(p-m)/(p+m)*100 if p+m else d(0) for p, m in zip(pdi, mdi)]
        adx = sum(dx[:14]) / 14
        for value in dx[14:]:
            adx = (adx * 13 + value) / 14
        bottom, top = min(low), max(high)
        width = (top - bottom) / 20
        bins = [d(0)] * 20
        for h, l, c, v in zip(high, low, close, volume):
            index = min(19, int(((h+l+c)/3-bottom)/width))
            bins[index] += v
        poc = bins.index(max(bins))
        return dict(adx=adx, plusDi=pdi[-1], minusDi=mdi[-1], profileVolume=sum(volume),
                    profileLow=bottom+poc*width, profileHigh=bottom+(poc+1)*width,
                    profileShare=max(bins)/sum(volume)*100, mean15=sum(close[-20:])/20)


if __name__ == "__main__":
    result = expected()
    if len(sys.argv) > 1:
        actual = json.load(open(sys.argv[1], encoding="utf-8"))
        for key, value in result.items():
            assert abs(Decimal(str(actual[key])) - value) < Decimal("1e-9"), (key, actual[key], str(value))
        print(f"G8 studies: {len(result)} Decimal fields matched")
    else:
        print(json.dumps({k: str(v) for k, v in result.items()}, indent=2))
