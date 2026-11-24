#!/usr/bin/env python3
"""Cross-check synthetic G8 endpoint values with the existing G7 recalculator.

No vault, archive or network access. The C++ history test reads the same fixture;
--cpp-result additionally compares its optional result export field by field.
This checks endpoint valuation/partial scope/equity arithmetic, not ledger I/O,
connection mutation detection or the complete history selection implementation.
"""
import argparse
import json
from pathlib import Path

import recalc


def evaluate_case(case):
    valuations = []
    coverage = []
    for name in ("opening", "closing"):
        bundle = recalc.Bundle.load(case[name])
        point = recalc.value(bundle.snapshots, bundle.quotes, [], bundle.as_of_ms,
                             bundle.excluded_assets)
        assert point.subtotal is not None, f"{case['name']}: invalid subtotal"
        assert any(row.value is not None for row in point.rows), f"{case['name']}: no valued rows"
        coverage.append({(row.account_scope, row.component_id, row.asset,
                          row.state, row.excluded_by_user) for row in point.rows})
        valuations.append(point)
    opening, closing = valuations
    assert coverage[0] == coverage[1], f"{case['name']}: changed coverage"
    assert opening.complete == closing.complete, f"{case['name']}: changed completeness"
    assert closing.at > opening.at, f"{case['name']}: reversed endpoints"
    return {
        "available": True,
        "complete": opening.complete and closing.complete,
        "baseline_time_ms": opening.at,
        "opening_value": recalc.text(opening.subtotal),
        "closing_value": recalc.text(closing.subtotal),
        "difference": recalc.text(recalc.add(closing.subtotal, opening.subtotal.copy_negate())),
    }


def check(fixture, cpp_result=None):
    data = json.loads(Path(fixture).read_text())
    assert data["format"] == "keefi-g8-history-synthetic/1"
    computed = {}
    fields = 0
    for case in data["cases"]:
        assert case["name"] not in computed, "duplicate fixture name"
        actual = evaluate_case(case)
        assert actual == case["expected"], f"{case['name']}: fixture arithmetic mismatch"
        computed[case["name"]] = actual
        fields += len(actual)
    if cpp_result:
        cpp = json.loads(Path(cpp_result).read_text())
        assert cpp["format"] == "keefi-g8-history-crosscheck/1"
        assert cpp["cases"] == computed, "C++ / Python G8 result mismatch"
    return {"cases": len(computed), "fields": fields,
            "cpp_result_compared": bool(cpp_result), "matched": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path,
                        default=Path(__file__).with_name("g8-history-fixture.json"))
    parser.add_argument("--cpp-result", type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.fixture, args.cpp_result), sort_keys=True))


if __name__ == "__main__":
    main()
