#!/usr/bin/env python3
"""Pure Dev101 gate rules: preserve the Dev100 suite set and bound name changes."""
import hashlib
import re

# The 179 registered suffixes in the successful Dev100 gate are frozen evidence.
# Prefixes identify their historical and current registrations; no test is removed.
DEV100_TEST_SUFFIXES = (
    "account_result_layout",
    "asset_identity",
    "asset_share_card",
    "asset_share_integration",
    "assets",
    "assets_window",
    "background_tray",
    "balance_time",
    "binance",
    "binance_balance_stream",
    "binance_earn",
    "binance_usdm",
    "bithumb",
    "bithumb_trading",
    "broker_trading",
    "candle_aggregation",
    "chart_alert_engine",
    "chart_alert_feed",
    "chart_alerts_panel",
    "chart_design",
    "chart_display_warmup",
    "chart_drawings",
    "chart_grid",
    "chart_horizontal_alerts",
    "chart_indicator_picker",
    "chart_indicator_state",
    "chart_instrument_catalog",
    "chart_instrument_panel",
    "chart_instrument_panel_interaction",
    "chart_instrument_picker",
    "chart_market_context",
    "chart_phrase_pack",
    "chart_public_context",
    "chart_public_observations",
    "chart_share_card",
    "chart_share_dialog",
    "chart_strong_observations",
    "chart_studies",
    "chart_study_events",
    "chart_study_feed",
    "chart_study_help",
    "chart_study_preferences",
    "chart_study_request_pacer",
    "chart_study_search",
    "chart_time_axis",
    "chart_workspace",
    "coin_flow_protocol",
    "coin_flow_source",
    "coin_flow_studies",
    "coin_flow_study_runner",
    "coinbase_quotes",
    "collector_runtime",
    "collector_store",
    "combo_popup",
    "connection_form",
    "d4_design",
    "d4_legacy_design",
    "d5_design",
    "d5_dialogs",
    "derivative_presentation",
    "derivatives_portfolio",
    "display_currency",
    "display_fx",
    "earnings_calendar",
    "explore_layout",
    "explore_metric_card",
    "explore_presentation",
    "extended_chart_studies",
    "fonts",
    "fund_flows",
    "futures_trading",
    "fx_history",
    "holding_chart_panel",
    "holding_market_integration",
    "holding_market_resolver",
    "hyperliquid_read",
    "info_button",
    "integration",
    "keefi_install_scripts",
    "kis",
    "kis_catalog",
    "kiwoom",
    "kiwoom_market_protocol",
    "kiwoom_market_stream",
    "kiwoom_market_view",
    "kiwoom_request_budget",
    "kiwoom_trading",
    "local_window",
    "ls",
    "ls_trading",
    "market_breadth_monitor",
    "market_candles",
    "market_candles_view",
    "market_compare",
    "market_compare_period",
    "market_compare_period_view",
    "market_compare_source",
    "market_compare_view",
    "market_flow_history",
    "market_flow_page",
    "market_flow_presentation",
    "market_map",
    "market_metrics",
    "market_metrics_view",
    "market_overview",
    "market_overview_view",
    "market_studies",
    "money_display",
    "movement_color_policy",
    "movement_presentations",
    "official_calendar",
    "official_notices",
    "okx",
    "okx_events",
    "okx_futures_read",
    "okx_trading",
    "portfolio",
    "portfolio_checkpoint",
    "portfolio_comparison",
    "portfolio_contribution",
    "portfolio_day_view",
    "portfolio_detail",
    "portfolio_history",
    "portfolio_presentation",
    "price_archive",
    "product_identity",
    "profile_migration",
    "profile_migration_dialog",
    "profile_path_guards",
    "profile_settings",
    "provider_reconciliation",
    "public_coin_identity",
    "public_history",
    "public_market_catalog",
    "public_stream",
    "ranking_futures_feed",
    "ranking_studies",
    "ranking_study_runner",
    "rapid_move_monitor",
    "read_only",
    "read_only_binary",
    "read_only_receipt",
    "record_transfer",
    "record_transfer_window",
    "reproducible_macos",
    "retired_connections",
    "row_delegates",
    "security",
    "solana_tokens",
    "stock_chart_rules",
    "stock_chart_source",
    "stock_period_levels",
    "swap_journal",
    "system_notification_sink",
    "theme",
    "theme_headless",
    "theme_source",
    "today_btc_premium",
    "today_calendar",
    "today_premium_window",
    "toss",
    "toss_market_stream",
    "toss_market_view",
    "trading",
    "transport",
    "unlock_loading",
    "upbit",
    "upbit_announcements",
    "usability_cleanup",
    "valuation_bundle",
    "valuation_preferences",
    "valuation_scope_ui",
    "vault",
    "vault_gate",
    "vault_keyboard",
    "vault_keyboard_roundtrip",
    "vault_layout",
    "watchlist_workspace_list",
    "world_markets",
)
LEGACY_TEST_PREFIX = "finix_"
CURRENT_TEST_PREFIX = "keefi_"
PROTECTED_RENAME_RULES = (("finix", "keefi"), ("FINIX", "KEEFI"), ("Finix", "Keefi"))


def required_test_names():
    return {CURRENT_TEST_PREFIX + suffix for suffix in DEV100_TEST_SUFFIXES}


def registered_test_preservation(names):
    names = list(names)
    if len(DEV100_TEST_SUFFIXES) != 179 or len(set(DEV100_TEST_SUFFIXES)) != 179:
        raise ValueError("The frozen Dev100 test inventory is invalid")
    if len(names) != len(set(names)):
        raise ValueError("Duplicate current CTest registration")
    if any(not isinstance(name, str) or not re.fullmatch(r"keefi_\w+", name) for name in names):
        raise ValueError("Every current suite must have a KEEFI registration")
    required = required_test_names()
    missing = sorted(required - set(names))
    if missing:
        raise ValueError("Missing migrated Dev100 suite: " + ", ".join(missing))
    return {
        "baseline_count": 179,
        "mapped_count": len(required),
        "registered_count": len(names),
        "mapping": [{"legacy": LEGACY_TEST_PREFIX + suffix,
                     "current": CURRENT_TEST_PREFIX + suffix}
                    for suffix in DEV100_TEST_SUFFIXES],
        "additional": sorted(set(names) - required),
    }


def normalized_rebrand_bytes(baseline):
    result = bytes(baseline)
    for old, new in PROTECTED_RENAME_RULES:
        result = result.replace(old.encode("ascii"), new.encode("ascii"))
    return result


def protected_rebrand_record(baseline, current):
    expected = normalized_rebrand_bytes(baseline)
    if bytes(current) != expected:
        raise ValueError("Protected source differs beyond the three approved name substitutions")
    digest = lambda value: hashlib.sha256(value).hexdigest()
    return {"baseline_sha256": digest(bytes(baseline)), "sha256": digest(bytes(current)),
            "expected_normalized_sha256": digest(expected),
            "approved_identity_substitutions": [list(rule) for rule in PROTECTED_RENAME_RULES],
            "identical_after_approved_substitutions": True}


def approved_format_document_record(baseline, current, expected_sha256, authorization):
    if not isinstance(expected_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise ValueError("The approved new format document needs its exact SHA-256")
    if not isinstance(authorization, str) or not authorization.strip():
        raise ValueError("Explicit storage-format transition authorization is missing")
    actual = hashlib.sha256(bytes(current)).hexdigest()
    if actual != expected_sha256:
        raise ValueError("The approved new format document changed")
    return {"baseline_sha256": hashlib.sha256(bytes(baseline)).hexdigest(),
            "sha256": actual, "unchanged_from_baseline": bytes(baseline) == bytes(current),
            "approved_storage_format_transition": True,
            "authorization": authorization}
