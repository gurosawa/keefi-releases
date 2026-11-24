#!/usr/bin/env python3
"""Independent public-candle oracle for the written KEEFI C4 contract."""
import argparse
from decimal import Decimal, InvalidOperation, ROUND_FLOOR, ROUND_HALF_UP, localcontext
import hashlib
import json
import math
from pathlib import Path
import re

INTERVALS = (60000, 300000, 900000, 3600000, 14400000, 86400000)
HOUR = 3600000
NUMBER = re.compile(r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?", re.ASCII)
ZERO = Decimal(0)
# Display-only strong positive shape match; not a significance or forecast claim.
SIMILARITY_MINIMUM_CORRELATION = Decimal("0.80")
EXACT_COUNTS = {"intervalMs", "observations", "currentRunBars", "overlap", "score", "queryBars",
                "followingBars", "searchBars", "eligibleCandidates", "overlapDropped", "samples",
                "upPaths", "downPaths", "flatPaths", "unresolvedSignals", "overlappingSignals", "up", "down", "flat"}


def _pullback_empty():
    return {"available": False, "inBox": False, "reason": "연속 완료 봉 21개 필요",
            "waveLowAtMs": 0, "waveHighAtMs": 0, "waveLow": None, "waveHigh": None,
            "boxLow": None, "boxHigh": None, "atr": None, "overlap": 0, "score": 0, "evidence": []}


def _similarity_empty():
    return {"available": False, "volatilityNormalized": False, "reason": "완전한 과거 구간과 이후 경로 필요",
            "queryBars": 20, "followingBars": 20, "searchBars": 0, "eligibleCandidates": 0,
            "overlapDropped": 0, "samples": 0, "upPaths": 0, "downPaths": 0, "flatPaths": 0,
            "searchFromMs": 0, "searchToMs": 0, "paths": [], "mean": [], "minimum": [], "maximum": []}


def _scorecard_empty(interval):
    horizon = HOUR if interval in (60000, 300000) else 4 * HOUR if interval == 900000 else 24 * HOUR if interval in INTERVALS else 0
    return {"available": False, "costsExcluded": True, "reason": "평가를 마친 신호 표본 없음",
            "horizonMs": horizon, "samples": 0, "unresolvedSignals": 0, "overlappingSignals": 0,
            "up": 0, "down": 0, "flat": 0, "meanReturnPercent": None, "outcomes": []}


def _empty(interval):
    pullback, similarity, scorecard = _pullback_empty(), _similarity_empty(), _scorecard_empty(interval)
    for section in (pullback, similarity, scorecard):
        section["reason"] = ""
    scorecard["horizonMs"] = 0
    return {"available": False, "reason": "완료 캔들 입력 검증 실패", "intervalMs": interval,
            "lastCompletedAtMs": 0, "observations": 0, "currentRunBars": 0,
            "pullback": pullback, "similarity": similarity, "cloud": [],
            "volumes": [], "signals": [], "scorecard": scorecard}


def _validated(document):
    if not isinstance(document, dict) or set(document) != {"intervalMs", "candles"}:
        raise ValueError("Only a public interval and completed candles are accepted")
    interval, input_rows = document["intervalMs"], document["candles"]
    if type(interval) is not int or interval not in INTERVALS or not isinstance(input_rows, list) or not 1 <= len(input_rows) <= 200:
        raise ValueError("Unsupported interval or input budget")
    rows, previous = [], 0
    required = {"startsAtMs", "open", "high", "low", "close", "volume"}
    for source in input_rows:
        if not isinstance(source, dict) or not required <= set(source) or set(source) - required - {"lastStoredTickMs"}:
            raise ValueError("Unknown or missing candle fields")
        at = source["startsAtMs"]
        future_budget = max(20 * interval, interval + _scorecard_empty(interval)["horizonMs"])
        if type(at) is not int or at <= previous or at % interval or at > (1 << 63) - 1 - future_budget:
            raise ValueError("Candle times must be positive, aligned and strictly ascending")
        if "lastStoredTickMs" in source and (type(source["lastStoredTickMs"]) is not int or not 0 <= source["lastStoredTickMs"] < 1 << 63):
            raise ValueError("Invalid optional source timestamp")
        row = {"at": at, "sourceVolume": source["volume"]}
        for key in ("open", "high", "low", "close", "volume"):
            text = source[key]
            if not isinstance(text, str) or not NUMBER.fullmatch(text):
                raise ValueError("Only exact decimal text is accepted")
            number = Decimal(text)
            if not number.is_finite() or number > Decimal("1e100") or number < (ZERO if key == "volume" else Decimal("1e-100")):
                raise ValueError("Candle number outside the display contract")
            row[key] = number
        if row["high"] < max(row["open"], row["close"]) or row["low"] > min(row["open"], row["close"]):
            raise ValueError("Invalid OHLC bounds")
        rows.append(row)
        previous = at
    return rows


def _runs(rows, interval):
    runs = []
    for row in rows:
        if not runs or row["at"] - runs[-1][-1]["at"] != interval:
            runs.append([])
        runs[-1].append(row)
    return runs


def _pullback(run):
    result = _pullback_empty()
    if len(run) < 21:
        return result
    ranges = [max(r["high"] - r["low"], abs(r["high"] - prior["close"]), abs(r["low"] - prior["close"]))
              for prior, r in zip(run, run[1:])]
    atr = sum(ranges[:20], ZERO) / 20
    for value in ranges[20:]:
        atr = (19 * atr + value) / 20
    result["atr"] = atr
    recent = run[-20:]
    mean = sum((r["close"] for r in recent), ZERO) / 20
    variance = sum(((r["close"] - mean) ** 2 for r in recent), ZERO) / 20
    levels = [("sma20", "이평20", mean), ("bbLower20", "볼린저 하단", mean - 2 * variance.sqrt())]
    profile = run[-96:]
    if sum((r["volume"] for r in profile), ZERO) > 0:
        low, high = min(r["low"] for r in profile), max(r["high"] for r in profile)
        price = low
        if high > low:
            step, bins = (high - low) / 20, [ZERO] * 20
            for r in profile:
                typical = (r["high"] + r["low"] + r["close"]) / 3
                index = max(0, min(19, int(((typical - low) / step).to_integral_value(rounding=ROUND_FLOOR))))
                bins[index] += r["volume"]
            winner = max(range(20), key=lambda index: bins[index])
            price = low + (Decimal(winner) + Decimal("0.5")) * step
        levels.append(("profilePoc96", "거래량 대표가격 근사", price))
    mean_low = sum((r["low"] for r in recent), ZERO) / 20
    numerator = sum(((Decimal(i) - Decimal("9.5")) * (r["low"] - mean_low) for i, r in enumerate(recent)), ZERO)
    levels.append(("lowRegression20", "저가 회귀선20", mean_low + Decimal("9.5") * numerator / 665))
    half = atr / 2
    result["evidence"] = [{"id": key, "label": label, "price": value} for key, label, value in levels
                          if abs(run[-1]["close"] - value) <= half]
    result["overlap"] = len(result["evidence"])
    result["score"] = min(10, max(0, int((Decimal(result["overlap"]) * Decimal("2.5")).to_integral_value(rounding=ROUND_HALF_UP))))
    window = run[-60:]
    low_index = min(range(len(window) - 1), key=lambda index: window[index]["low"])
    candidates = range(low_index + 1, len(window) - 1)
    if not candidates:
        result["reason"] = "최신 봉 이전의 상승 파동 고점 없음"
        return result
    high_index = max(candidates, key=lambda index: window[index]["high"])
    low, high = window[low_index]["low"], window[high_index]["high"]
    if atr <= 0 or high <= low or high - low < 2 * atr:
        result["reason"] = "상승 파동의 가격 범위 부족"
        return result
    result.update(waveLowAtMs=window[low_index]["at"], waveHighAtMs=window[high_index]["at"], waveLow=low, waveHigh=high)
    if not result["evidence"]:
        result["reason"] = "현재 종가 주변의 겹친 근거 없음"
        return result
    box_low = max(low, min(e["price"] for e in result["evidence"]) - half)
    box_high = min(high, max(e["price"] for e in result["evidence"]) + half)
    if box_low > box_high:
        result["reason"] = "파동 범위와 근거 가격대가 만나지 않음"
        return result
    result.update(available=True, reason="", boxLow=box_low, boxHigh=box_high,
                  inBox=box_low <= run[-1]["close"] <= box_high)
    return result


def _similarity(run, interval):
    result = _similarity_empty()
    if len(run) < 20:
        return result
    query_start = len(run) - 20
    result.update(searchBars=query_start, searchFromMs=run[0]["at"], searchToMs=run[query_start]["at"])
    returns = [(b["close"] / a["close"]).ln() for a, b in zip(run, run[1:])]
    query = returns[query_start:query_start + 19]
    query_mean = sum(query, ZERO) / 19
    centered_query = [v - query_mean for v in query]
    query_variance = sum((v * v for v in centered_query), ZERO)
    if query_variance <= Decimal("1e-24"):
        return result
    candidates = []
    for start in range(max(0, query_start - 39)):
        candidate = returns[start:start + 19]
        candidate_mean = sum(candidate, ZERO) / 19
        centered = [v - candidate_mean for v in candidate]
        variance = sum((v * v for v in centered), ZERO)
        if variance <= Decimal("1e-24"):
            continue
        covariance = sum((a * b for a, b in zip(centered, centered_query)), ZERO)
        correlation = max(Decimal(-1), min(Decimal(1), covariance / (variance * query_variance).sqrt()))
        rank = correlation.quantize(Decimal("0.000000000001"), rounding=ROUND_HALF_UP)
        candidates.append((start, rank, correlation))
    result["eligibleCandidates"] = len(candidates)
    retained = []
    for start, rank, correlation in sorted(candidates, key=lambda x: (-x[1], x[0])):
        if correlation < SIMILARITY_MINIMUM_CORRELATION:
            continue
        if any(start < prior + 40 and prior < start + 40 for prior, _ in retained):
            result["overlapDropped"] += 1
            continue
        if len(retained) < 3:
            retained.append((start, correlation))
    current = run[-1]
    for start, correlation in retained:
        candidate_end, following = run[start + 19], run[start + 20:start + 40]
        points = [{"atMs": current["at"], "price": current["close"]}]
        points.extend({"atMs": current["at"] + step * interval,
                       "price": current["close"] * r["close"] / candidate_end["close"]}
                      for step, r in enumerate(following, 1))
        result["paths"].append({"fromMs": run[start]["at"], "toMs": candidate_end["at"] + interval,
                                "followingToMs": following[-1]["at"] + interval,
                                "correlation": correlation, "points": points})
        direction = "upPaths" if following[-1]["close"] > candidate_end["close"] else "downPaths" if following[-1]["close"] < candidate_end["close"] else "flatPaths"
        result[direction] += 1
    result["samples"] = len(retained)
    if not retained and len(run) >= 60:
        result["reason"] = "충분히 닮은 과거 구간이 없어요"
    if retained:
        result.update(available=True, reason="")
        for step in range(21):
            prices = [path["points"][step]["price"] for path in result["paths"]]
            at = current["at"] + step * interval
            result["mean"].append({"atMs": at, "price": sum(prices, ZERO) / len(prices)})
            result["minimum"].append({"atMs": at, "price": min(prices)})
            result["maximum"].append({"atMs": at, "price": max(prices)})
    return result


def _cloud(runs):
    result = []
    for run in runs:
        fast = slow = None
        for count, r in enumerate(run, 1):
            if count == 12:
                fast = sum((bar["close"] for bar in run[:12]), ZERO) / 12
            elif count > 12:
                fast += Decimal(2) * (r["close"] - fast) / 13
            if count == 26:
                slow = sum((bar["close"] for bar in run[:26]), ZERO) / 26
            elif count > 26:
                slow += Decimal(2) * (r["close"] - slow) / 27
            if fast is not None and slow is not None:
                result.append({"atMs": r["at"], "fast": fast, "slow": slow})
    return result


def _signals(rows, interval):
    result, run, emitted = [], [], set()
    previous = _pullback_empty()
    for r in rows:
        if run and r["at"] - run[-1]["at"] != interval:
            run = []
        run.append(r)
        pullback = _pullback(run)
        wave = pullback["waveLowAtMs"]
        was_inside = previous["available"] and previous["inBox"] and previous["waveLowAtMs"] == wave
        if pullback["available"] and pullback["inBox"] and pullback["overlap"] >= 2 and not was_inside and wave not in emitted:
            result.append({"atMs": r["at"] + interval, "waveLowAtMs": wave,
                           "entryClose": r["close"], "score": pullback["score"]})
            emitted.add(wave)
        previous = pullback
    return result


def _scorecard(rows, signals, interval):
    result = _scorecard_empty(interval)
    horizon = result["horizonMs"]
    by_completion = {row["at"] + interval: i for i, row in enumerate(rows)}
    prior_windows = []
    for signal in signals:
        at = signal["atMs"]
        if any(at < prior + horizon and prior < at + horizon for prior in prior_windows):
            result["overlappingSignals"] += 1
        prior_windows.append(at)
        entry_index, target_index = by_completion[at], by_completion.get(at + horizon)
        if target_index is None or any(rows[i]["at"] - rows[i - 1]["at"] != interval for i in range(entry_index + 1, target_index + 1)):
            result["unresolvedSignals"] += 1
            continue
        change = 100 * (rows[target_index]["close"] / signal["entryClose"] - 1)
        result["outcomes"].append({"signalAtMs": at, "evaluationAtMs": at + horizon, "returnPercent": change})
        result["up" if change > 0 else "down" if change < 0 else "flat"] += 1
    result["samples"] = len(result["outcomes"])
    if result["samples"]:
        result.update(available=True, reason="", meanReturnPercent=sum((x["returnPercent"] for x in result["outcomes"]), ZERO) / result["samples"])
    return result


def _plain(value):
    if isinstance(value, Decimal):
        result = float(value)
        if not math.isfinite(result):
            raise ValueError("Non-finite result")
        return result
    if isinstance(value, dict):
        return {key: _plain(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_plain(item) for item in value]
    return value


def calculate(document):
    """Calculate the canonical camelCase DTO from completed synthetic candles."""
    interval = document.get("intervalMs", 0) if isinstance(document, dict) else 0
    if type(interval) is not int:
        interval = 0
    result = _empty(interval)
    with localcontext() as context:
        context.prec = 100
        try:
            rows = _validated(document)
            runs = _runs(rows, interval)
            signals = _signals(rows, interval)
            result.update(available=True, reason="", observations=len(rows), currentRunBars=len(runs[-1]),
                          lastCompletedAtMs=rows[-1]["at"] + interval, pullback=_pullback(runs[-1]),
                          similarity=_similarity(runs[-1], interval), cloud=_cloud(runs), signals=signals,
                          volumes=[{"atMs": row["at"], "volume": row["sourceVolume"]} for row in rows],
                          scorecard=_scorecard(rows, signals, interval))
            return _plain(result)
        except (ValueError, InvalidOperation, OverflowError, ArithmeticError):
            return _empty(interval)


def compare(calculated, actual):
    """Compare structural fields exactly and numeric values at contract tolerance."""
    differences, compared = [], 0

    def different(path, expected, observed, reason):
        differences.append({"path": path, "expected": expected, "actual": observed, "reason": reason})

    def visit(expected, observed, path, key=""):
        nonlocal compared
        if isinstance(expected, dict) and isinstance(observed, dict):
            if set(expected) != set(observed):
                different(path, sorted(expected), sorted(observed), "Object keys differ")
            for child in sorted(set(expected) & set(observed)):
                visit(expected[child], observed[child], path + "." + child, child)
            return
        if isinstance(expected, list) and isinstance(observed, list):
            if len(expected) != len(observed):
                different(path, len(expected), len(observed), "Array length differs")
            for i, (left, right) in enumerate(zip(expected, observed)):
                visit(left, right, f"{path}[{i}]", key)
            return
        compared += 1
        if key == "reason" and isinstance(expected, str) and isinstance(observed, str):
            if bool(expected) != bool(observed):
                different(path, bool(expected), bool(observed), "Reason availability differs; prose is not compared")
            return
        if isinstance(expected, bool) or isinstance(observed, bool):
            if type(expected) is not bool or type(observed) is not bool or expected != observed:
                different(path, expected, observed, "Boolean differs")
            return
        if isinstance(expected, (int, float, Decimal)) and isinstance(observed, (int, float, Decimal)):
            try:
                left, right = Decimal(str(expected)), Decimal(str(observed))
                if not left.is_finite() or not right.is_finite():
                    raise InvalidOperation
                if key.endswith("Ms") or key in EXACT_COUNTS:
                    if left != right:
                        different(path, expected, observed, "Timestamp/count differs")
                elif abs(left - right) > Decimal("1e-9") + Decimal("1e-10") * max(abs(left), abs(right)):
                    different(path, expected, observed, "Numeric tolerance exceeded")
            except InvalidOperation:
                different(path, str(expected), str(observed), "Non-finite numeric value")
            return
        if type(expected) is not type(observed) or expected != observed:
            different(path, expected, observed, "Exact field differs")

    visit(calculated, actual, "$")
    return {"equal": not differences, "comparedFields": compared, "differences": differences,
            "numericTolerance": {"absolute": "1e-9", "relative": "1e-10"},
            "reasonPolicy": "Key and empty/nonempty state only; prose excluded"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--cpp", type=Path, help="Canonical C++ result for the same public fixture")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    raw_input = args.input.read_bytes()
    if len(raw_input) > 2 * 1024 * 1024:
        parser.error("Public fixture exceeds the 2 MiB input limit")
    calculated = calculate(json.loads(raw_input))
    output = {"calculated": calculated, "inputSha256": hashlib.sha256(raw_input).hexdigest()}
    if args.cpp:
        raw_cpp = args.cpp.read_bytes()
        output["comparison"] = compare(calculated, json.loads(raw_cpp))
        output["cppSha256"] = hashlib.sha256(raw_cpp).hexdigest()
    encoded = json.dumps(output, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0 if "comparison" not in output or output["comparison"]["equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
