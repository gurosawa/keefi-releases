#!/usr/bin/env python3
"""Contract-first synthetic tests; no account, profile, or network inputs."""
import copy
import importlib.util
import math
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("g8_chart_studies", Path(__file__).with_name("g8_chart_studies.py"))
oracle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(oracle)

START = 1792627200000  # Synthetic UTC midnight, never an account observation.
HOUR = 3600000
PATTERN = [0, 2, 1, 4, 3, 7, 5, 8, 4, 9, 6, 10, 8, 12, 9, 13, 11, 15, 12, 16]


def row(index, close="100", high=None, low=None, volume="1.000", interval=HOUR):
    return {"startsAtMs": START + index * interval, "open": close,
            "high": high if high is not None else close,
            "low": low if low is not None else close, "close": close, "volume": volume}


def document(rows, interval=HOUR):
    return {"intervalMs": interval, "candles": rows}


def periodic_rows(count, interval=HOUR):
    return [row(i, str(100 + PATTERN[i % 20]), interval=interval) for i in range(count)]


def signal_rows(count=50, interval=HOUR):
    result = [row(i, "100", "101", "99", interval=interval) for i in range(20)]
    result.append(row(20, "110", "111", "109", "2", interval))
    result.extend(row(i, "100.8", "101.8", "99.8", "3", interval) for i in range(21, count))
    return result


class IndependentChartStudies(unittest.TestCase):
    def valid(self, rows, interval=HOUR):
        result = oracle.calculate(document(rows, interval))
        self.assertTrue(result["available"], result.get("reason"))
        return result

    def test_flat_input_keeps_volume_and_hand_calculated_cloud(self):
        rows = [row(i) for i in range(40)]
        result = self.valid(rows)
        self.assertEqual(result["observations"], 40)
        self.assertEqual(result["currentRunBars"], 40)
        self.assertEqual(result["lastCompletedAtMs"], START + 40 * HOUR)
        self.assertEqual(len(result["cloud"]), 15)
        self.assertEqual(result["cloud"][0], {"atMs": START + 25 * HOUR, "fast": 100, "slow": 100})
        self.assertEqual(result["volumes"], [{"atMs": r["startsAtMs"], "volume": "1.000"} for r in rows])
        self.assertEqual(result["pullback"]["atr"], 0)
        self.assertEqual(result["pullback"]["score"], 10)
        self.assertFalse(result["pullback"]["available"])
        self.assertIsNone(result["pullback"]["waveLow"])
        self.assertIsNone(result["pullback"]["boxLow"])
        self.assertFalse(result["similarity"]["available"])

    def test_four_supports_include_first_maximum_volume_bin(self):
        result = self.valid([row(i, "100", "110", "90") for i in range(40)])
        pullback = result["pullback"]
        self.assertEqual(pullback["atr"], 20)
        self.assertEqual(pullback["overlap"], 4)
        self.assertEqual(pullback["score"], 10)
        self.assertEqual([(x["id"], x["price"]) for x in pullback["evidence"]],
                         [("sma20", 100), ("bbLower20", 100), ("profilePoc96", 100.5), ("lowRegression20", 90)])

    def test_one_near_support_rounds_half_up_to_three(self):
        rows = [row(i, "95" if i % 2 == 0 else "105", low="90", volume="0") for i in range(20)]
        rows.append(row(20, "100", low="90", volume="0"))
        pullback = self.valid(rows)["pullback"]
        self.assertEqual([e["id"] for e in pullback["evidence"]], ["sma20"])
        self.assertEqual(pullback["overlap"], 1)
        self.assertEqual(pullback["score"], 3)

    def test_gap_restarts_warmup_without_erasing_earlier_cloud_points(self):
        rows = [row(i) for i in range(40)] + [row(i, "200") for i in range(50, 60)]
        result = self.valid(rows)
        self.assertEqual(result["observations"], 50)
        self.assertEqual(result["currentRunBars"], 10)
        self.assertIsNone(result["pullback"]["atr"])
        self.assertEqual(len(result["cloud"]), 15)
        self.assertLess(result["cloud"][-1]["atMs"], START + 50 * HOUR)

    def test_similarity_suppresses_full_windows_and_rebases_actual_paths(self):
        result = self.valid(periodic_rows(200))
        similar = result["similarity"]
        self.assertTrue(similar["available"])
        self.assertEqual(similar["searchBars"], 180)
        self.assertEqual(similar["eligibleCandidates"], 141)
        self.assertEqual(similar["samples"], 3)
        self.assertEqual(similar["overlapDropped"], 9)  # Only candidates meeting r >= 0.80 enter overlap suppression.
        self.assertEqual([p["fromMs"] for p in similar["paths"]], [START, START + 40 * HOUR, START + 80 * HOUR])
        self.assertEqual((similar["upPaths"], similar["downPaths"], similar["flatPaths"]), (0, 0, 3))
        self.assertFalse(similar["volatilityNormalized"])
        for path in similar["paths"]:
            self.assertAlmostEqual(path["correlation"], 1)
            self.assertEqual(len(path["points"]), 21)
            self.assertEqual(path["points"][0], {"atMs": START + 199 * HOUR, "price": 116})
            self.assertEqual(path["points"][1], {"atMs": START + 200 * HOUR, "price": 100})
            self.assertEqual(path["points"][-1], {"atMs": START + 219 * HOUR, "price": 116})
        self.assertEqual(similar["mean"], similar["minimum"])
        self.assertEqual(similar["mean"], similar["maximum"])

    def test_minimum_similarity_rejects_weak_and_opposite_paths(self):
        for correlation in (-1.0, 0.0, 0.5, 0.799999, 0.800001, 1.0):
            with self.subTest(correlation=correlation):
                rows = [row(i) for i in range(60)]
                past = query = 100.0
                for i in range(19):
                    angle = 2 * math.pi * i / 19
                    a, b = math.sin(angle), math.cos(angle)
                    past *= math.exp(0.01 * a)
                    query *= math.exp(0.01 * (correlation * a + math.sqrt(1 - correlation * correlation) * b))
                    rows[i + 1] = row(i + 1, f"{past:.12f}")
                    rows[i + 41] = row(i + 41, f"{query:.12f}")
                similar = self.valid(rows)["similarity"]
                self.assertEqual(similar["eligibleCandidates"], 1)
                self.assertEqual(similar["available"], correlation >= 0.8)
                if correlation >= 0.8:
                    self.assertAlmostEqual(similar["paths"][0]["correlation"], correlation)
                else:
                    self.assertEqual(similar["reason"], "충분히 닮은 과거 구간이 없어요")
                    for key in ("paths", "mean", "minimum", "maximum"):
                        self.assertEqual(similar[key], [])

    def test_incomplete_following_is_excluded_and_touching_query_is_allowed(self):
        before = self.valid(periodic_rows(59))["similarity"]
        self.assertEqual(before["eligibleCandidates"], 0)
        self.assertFalse(before["available"])
        self.assertEqual(before["paths"], [])
        boundary = self.valid(periodic_rows(60))["similarity"]
        self.assertEqual(boundary["eligibleCandidates"], 1)
        self.assertEqual(boundary["samples"], 1)
        self.assertEqual(boundary["paths"][0]["followingToMs"], boundary["searchToMs"])

    def test_first_box_entry_is_once_per_wave_and_evaluates_exact_horizon(self):
        result = self.valid(signal_rows())
        self.assertEqual(result["signals"], [{"atMs": START + 22 * HOUR, "waveLowAtMs": START,
                                              "entryClose": 100.8, "score": 8}])
        scorecard = result["scorecard"]
        self.assertTrue(scorecard["available"])
        self.assertEqual(scorecard["horizonMs"], 24 * HOUR)
        self.assertEqual((scorecard["samples"], scorecard["unresolvedSignals"], scorecard["overlappingSignals"]), (1, 0, 0))
        self.assertEqual((scorecard["up"], scorecard["down"], scorecard["flat"]), (0, 0, 1))
        self.assertEqual(scorecard["outcomes"], [{"signalAtMs": START + 22 * HOUR,
                                                 "evaluationAtMs": START + 46 * HOUR, "returnPercent": 0}])
        self.assertEqual(scorecard["meanReturnPercent"], 0)
        self.assertTrue(scorecard["costsExcluded"])

    def test_intermediate_gap_leaves_signal_unresolved_even_with_target_present(self):
        rows = signal_rows()
        del rows[30]
        result = self.valid(rows)
        self.assertEqual(len(result["signals"]), 1)
        scorecard = result["scorecard"]
        self.assertFalse(scorecard["available"])
        self.assertEqual(scorecard["samples"], 0)
        self.assertEqual(scorecard["unresolvedSignals"], 1)
        self.assertIsNone(scorecard["meanReturnPercent"])
        self.assertEqual(scorecard["outcomes"], [])

    def test_future_perturbation_cannot_rewrite_earlier_prefix_signals(self):
        rows = signal_rows()
        prefix = self.valid(rows[:30])
        for i in range(30, len(rows)):
            rows[i] = row(i, str(500 + i), str(502 + i), str(499 + i))
        whole = self.valid(rows)
        earlier = [s for s in whole["signals"] if s["atMs"] <= START + 30 * HOUR]
        self.assertEqual(earlier, prefix["signals"])

    def test_overlapping_evaluation_windows_count_both_observed_returns(self):
        rows = signal_rows(60)
        rows[30] = row(30, "91", "92", "90")
        rows[31] = row(31, "111", "112", "110")
        rows[32:] = [row(i, "100", "101", "99") for i in range(32, 60)]
        result = self.valid(rows)
        self.assertEqual([s["atMs"] for s in result["signals"]], [START + 22 * HOUR, START + 33 * HOUR])
        scorecard = result["scorecard"]
        self.assertEqual((scorecard["samples"], scorecard["unresolvedSignals"], scorecard["overlappingSignals"]), (2, 0, 1))
        self.assertEqual((scorecard["up"], scorecard["down"], scorecard["flat"]), (0, 1, 1))
        self.assertAlmostEqual(scorecard["meanReturnPercent"], -25 / 63)

    def test_contract_numeric_extremes_and_coordinate_budget(self):
        for price in ("0." + "0" * 99 + "1", "1" + "0" * 100):
            with self.subTest(price_length=len(price)):
                self.valid([row(0, price)])
        leading_zero = document([row(0, "0100")])
        self.assertFalse(oracle.calculate(leading_zero)["available"])
        too_late = document([row(0)])
        too_late["candles"][0]["startsAtMs"] = (((1 << 63) - 1) // HOUR - 1) * HOUR
        self.assertFalse(oracle.calculate(too_late)["available"])

    def test_six_timeframes_use_the_written_evaluation_periods(self):
        for interval, horizon in [(60000, HOUR), (300000, HOUR), (900000, 4 * HOUR),
                                  (HOUR, 24 * HOUR), (4 * HOUR, 24 * HOUR), (24 * HOUR, 24 * HOUR)]:
            with self.subTest(interval=interval):
                result = self.valid(signal_rows(30, interval), interval)
                self.assertEqual(result["scorecard"]["horizonMs"], horizon)

    def test_invalid_private_forming_and_decimal_inputs_reject_atomically(self):
        baseline = document([row(i) for i in range(30)])
        cases = []
        for key in ("account", "forming", "alias"):
            altered = copy.deepcopy(baseline)
            altered[key] = "FAKE_PRIVATE_ONLY"
            cases.append(altered)
        for change in ("newline", "duplicate", "unaligned", "bounds", "too-many"):
            altered = copy.deepcopy(baseline)
            if change == "newline": altered["candles"][-1]["volume"] = "1\n"
            if change == "duplicate": altered["candles"][5] = copy.deepcopy(altered["candles"][4])
            if change == "unaligned": altered["candles"][5]["startsAtMs"] += 1
            if change == "bounds": altered["candles"][5]["low"] = "200"
            if change == "too-many": altered["candles"] = [row(i) for i in range(201)]
            cases.append(altered)
        for altered in cases:
            with self.subTest(keys=tuple(altered), rows=len(altered["candles"])):
                result = oracle.calculate(altered)
                self.assertFalse(result["available"])
                self.assertTrue(result["reason"])
                self.assertEqual(result["cloud"], [])
                self.assertEqual(result["volumes"], [])
                self.assertEqual(result["signals"], [])

    def test_invalid_canonical_result_keeps_uncomputed_nested_defaults(self):
        result = oracle.calculate(document([]))
        self.assertFalse(result["available"])
        self.assertTrue(result["reason"])
        self.assertEqual((result["observations"], result["currentRunBars"], result["lastCompletedAtMs"]), (0, 0, 0))
        for section in ("pullback", "similarity", "scorecard"):
            with self.subTest(section=section):
                self.assertEqual(result[section]["reason"], "")
        self.assertEqual(result["scorecard"]["horizonMs"], 0)
        self.assertIsNone(result["pullback"]["atr"])
        self.assertIsNone(result["scorecard"]["meanReturnPercent"])
        self.assertEqual((result["similarity"]["queryBars"], result["similarity"]["followingBars"]), (20, 20))
        self.assertTrue(result["scorecard"]["costsExcluded"])
        self.assertFalse(result["similarity"]["volatilityNormalized"])

    def test_comparison_uses_numeric_tolerance_but_exact_identity_and_volume(self):
        expected = {"available": True, "atMs": START, "price": 100.0, "volume": "1.000", "id": "sma20"}
        close = dict(expected, price=100.000000005)
        self.assertTrue(oracle.compare(expected, close)["equal"])
        for key, value in [("atMs", START + 1), ("price", 100.001), ("volume", "1"), ("id", "other")]:
            actual = dict(expected, **{key: value})
            self.assertFalse(oracle.compare(expected, actual)["equal"])


if __name__ == "__main__":
    unittest.main()
