#!/usr/bin/env python3
"""Compare an exported KEEFI valuation with the independent recalculator.

This checks KEEFI against its exported inputs, not against an exchange account.
Raw responses, aliases and credentials are never copied into the report.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import recalc
from legacy_formats import normalize_bundle


def required(obj, key, path):
    if not isinstance(obj, dict) or key not in obj:
        raise ValueError(f"required field missing: {path}/{key}")
    return obj[key]


def index_rows(rows, fields, path, repeated=False):
    if not isinstance(rows, list):
        raise ValueError(f"expected list: {path}")
    indexed = {}
    occurrences = {}
    for row in rows:
        key = tuple(required(row, field, path) for field in fields)
        if not all(isinstance(part, str) for part in key):
            raise ValueError(f"invalid identity: {path}")
        if repeated:
            ordinal = occurrences.get(key, 0)
            occurrences[key] = ordinal + 1
            key += (str(ordinal),)
        if key in indexed:
            raise ValueError(f"duplicate identity: {path}")
        indexed[key] = row
    return indexed


def compare_bundle(data):
    data = normalize_bundle(data)
    if not isinstance(data, dict) or data.get("format") != recalc.FORMAT:
        raise ValueError("expected keefi-valuation-bundle/1")
    app = required(data, "keefi_result", "")
    if not isinstance(app, dict):
        raise ValueError("invalid keefi_result")
    if app.get("screen", {}).get("displayed_saved_view"):
        raise ValueError("saved-view export requires its own captured inputs; refresh before exporting")
    expected = recalc.evaluate(recalc.Bundle.load(data))
    differences = []
    checks = 0

    def equal(path, actual, wanted, decimal=False):
        nonlocal checks
        checks += 1
        if decimal and actual is not None and wanted is not None:
            left, right = recalc.parse(actual), recalc.parse(wanted)
            same = left is not None and right is not None and left == right
        else:
            same = type(actual) is type(wanted) and actual == wanted
        if not same:
            differences.append({"path": path, "keefi": actual, "recalculated": wanted})

    def fields(path, actual, wanted, names, amounts=()):
        for name in names:
            equal(path + "/" + name, required(actual, name, path),
                  required(wanted, name, path), name in amounts)

    fields("valuation", required(app, "valuation", "keefi_result"), expected["valuation"],
           ("complete", "net_assets", "known_subtotal"), ("net_assets", "known_subtotal"))
    fields("screen", required(app, "screen", "keefi_result"), expected["screen"],
           ("complete", "label", "figure", "reference_total", "unvalued_groups", "reference_groups"),
           ("reference_total",))
    equal("screen/excluded_groups", app["screen"].get("excluded_groups", 0), expected["screen"].get("excluded_groups", 0))
    app_groups = index_rows(required(app, "groups", "keefi_result"), ("identity",), "groups")
    expected_groups = index_rows(expected["groups"], ("identity",), "groups")
    equal("groups/identities", sorted(app_groups), sorted(expected_groups))
    equal("groups/order", list(app_groups), list(expected_groups))
    for key in app_groups.keys() & expected_groups.keys():
        fields("groups/" + key[0], app_groups[key], expected_groups[key],
               ("asset", "quantity", "fresh_total", "partial", "share"), ("quantity", "fresh_total"))
    row_keys = ("identity", "account_scope", "component_id")
    app_rows = index_rows(required(app, "rows", "keefi_result"), row_keys, "rows", repeated=True)
    calculated_rows = [dict(row, identity=group["identity"], asset=group["asset"])
                       for group in expected["groups"] for row in group["rows"]]
    expected_rows = index_rows(calculated_rows, row_keys, "rows", repeated=True)
    equal("rows/identities", sorted(app_rows), sorted(expected_rows))
    for key in sorted(app_rows.keys() & expected_rows.keys()):
        fields("rows/" + "/".join(key), app_rows[key], expected_rows[key],
               ("asset", "kind", "quantity", "state", "presence", "value", "reference_value",
                "provider_pnl", "provider_pnl_currency"),
               ("quantity", "value", "reference_value", "provider_pnl"))
        equal("rows/" + "/".join(key) + "/excluded_by_user", app_rows[key].get("excluded_by_user", False),
              expected_rows[key].get("excluded_by_user", False))
    equal("allocation_total", required(app, "allocation_total", "keefi_result"),
          expected["allocation"]["total"], True)
    app_dust = required(app, "dust", "keefi_result")
    equal("dust/threshold", required(app_dust, "threshold", "dust"), expected["dust"]["threshold"], True)
    if expected["dust"]["threshold"] is not None:
        equal("dust/hidden", sorted(required(app_dust, "hidden", "dust")), sorted(expected["dust"]["hidden"]))
        equal("dust/hidden_total", required(app_dust, "hidden_total", "dust"), expected["dust"]["hidden_total"], True)
    equal("hide_unvalued", sorted(required(app, "hide_unvalued", "keefi_result")), sorted(expected["hide_unvalued"]))
    return {"format": "keefi-reconciliation-report/1", "as_of_ms": data["as_of_ms"],
            "matched": not differences, "checks": checks, "difference_count": len(differences),
            "differences": differences,
            "coverage": {"comparison": "keefi-export-vs-independent-recalculator",
                         "exchange_reconciliation": "not_checked",
                         "price_provenance": "not_checked",
                         "provider_totals": "not_checked",
                         "fields": ["valuation", "screen", "groups", "rows", "allocation_total", "dust", "hide_unvalued"]},
            "policy_assumptions": expected["assumptions"]}


def write_report(path, result):
    payload = (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as file:
        file.write(payload)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--output", type=Path, help="new private JSON report; existing files are preserved")
    args = parser.parse_args(argv)
    try:
        with args.bundle.open(encoding="utf-8") as file:
            data = json.load(file)
        result = compare_bundle(data)
        if args.output:
            write_report(args.output, result)
    except (OSError, ValueError, TypeError, KeyError, ArithmeticError) as error:
        # Avoid echoing malformed input values, filenames or raw private responses.
        print(f"Comparison unavailable ({type(error).__name__}). Check bundle schema and output path.", file=sys.stderr)
        return 2
    print(f"{'MATCH' if result['matched'] else 'DIFFERENT'}: {result['checks']} checks, "
          f"{result['difference_count']} differences; exchange reconciliation not checked.")
    return 0 if result["matched"] else 1


if __name__ == "__main__":
    sys.exit(main())
