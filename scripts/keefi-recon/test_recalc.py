#!/usr/bin/env python3
"""Each test checks one statement of docs/keefi/VALUATION-POLICY.md (cited by §)."""
import copy
import itertools
import random
import unittest
from decimal import Decimal
from pathlib import Path

import recalc

T = 1_800_000_000_000
SCOPE = "scope-a"


def ev(age=1_000, source_age=None, scope=SCOPE, complete=True):
    return {"source": "src", "url": "https://example.invalid", "scope": scope,
            "source_time_ms": 0 if source_age is None else T - source_age,
            "received_at_ms": T - age, "complete": complete}


def q(base, price, venue="upbit", counter="KRW", age=1_000, source_age=None):
    scope = (SCOPE if venue == 'account' else 'public:jupiter:price:' + base.removeprefix('solana:')
             if venue == 'jupiter' else 'public:upbit:' + counter + '-' + base if venue == 'upbit'
             else 'public:binance:' + base + counter)
    return {"base": base, "quote": counter, "price": price, "venue": venue,
            "evidence": ev(age, age if source_age is None else source_age, scope=scope)}


def c(id_, asset, quantity, kind="Holding", group="spot", **extra):
    return {"id": id_, "asset": asset, "kind": kind, "quantity": quantity, "valuation_group": group, **extra}


def s(components, scope=SCOPE, market="spot", conn="c1", age=1_000, complete=True, env="live"):
    return {"account_scope": scope, "connection_id": conn, "provider": "p", "environment": env,
            "market": market, "time_ms": T - age, "balance_complete": complete,
            "evidence": ev(age, scope=scope), "components": components}


def run(snapshots, quotes=(), refs=(), connections=None, at=None):
    if connections is None:
        connections = [{"id": x["connection_id"], "alias": x["connection_id"], "environment": "live", "state": "complete"}
                       for x in snapshots]
    bundle = recalc.Bundle.load({"format": recalc.FORMAT, "as_of_ms": T, "connections": connections,
                                 "snapshots": snapshots, "quotes": list(quotes), "reference_quotes": list(refs)})
    return recalc.evaluate(bundle, at)


class G7Policy(unittest.TestCase):
    def test_public_price_clock_skew_keeps_original_times(self):
        for lead in (0, 3, 19, 35, 1999, 2000, 2001):
            for cross in (False, True):
                with self.subTest(lead=lead, cross=cross):
                    direct, coin, fx = q('PUMP', '3'), q('PUMP', '0.003', 'binance', 'USDT'), q('USDT', '1000')
                    skewed = fx if cross else direct
                    skewed['evidence']['received_at_ms'] -= lead
                    original = copy.deepcopy(skewed['evidence'])
                    result = run([s([c('p', 'PUMP', '2')])], [coin, fx] if cross else [direct])
                    actual = row(result, 'exchange:PUMP')
                    self.assertEqual(actual['state'], 'Fresh' if lead <= 2000 else 'Unvalued')
                    if lead <= 2000:
                        self.assertEqual(actual['value'], '6')
                        if cross:
                            self.assertIn(original, [leg['evidence'] for leg in actual['price']['legs']])
                        else:
                            self.assertEqual(actual['price']['evidence'], original)
                    self.assertEqual(skewed['evidence'], original)

    def test_clock_skew_does_not_extend_freshness_or_reference_windows(self):
        cases = [('source-at-asof', 0, 35, 'Fresh'), ('future-source', -1, 34, 'Unvalued'),
                 ('future-receipt', 1000, -1, 'Unvalued'), ('fresh-source-boundary', 180000, 180000, 'Fresh'),
                 ('fresh-receipt-boundary', 179965, 180000, 'Fresh'), ('fresh-receipt-expired', 179966, 180001, 'Reference'),
                 ('reference-source-boundary', 86400000, 86400000, 'Reference'),
                 ('reference-receipt-boundary', 86399965, 86400000, 'Reference'),
                 ('reference-receipt-expired', 86399966, 86400001, 'Unvalued')]
        for name, source_age, receipt_age, expected in cases:
            for cross in (False, True):
                with self.subTest(case=name, cross=cross):
                    direct, coin, fx = q('PUMP', '3'), q('PUMP', '0.003', 'binance', 'USDT'), q('USDT', '1000')
                    skewed = fx if cross else direct
                    skewed['evidence']['source_time_ms'] = T - source_age
                    skewed['evidence']['received_at_ms'] = T - receipt_age
                    original = copy.deepcopy(skewed['evidence'])
                    quotes = [coin, fx] if cross else [direct]
                    actual = row(run([s([c('p', 'PUMP', '2')])], quotes, refs=quotes), 'exchange:PUMP')
                    self.assertEqual(actual['state'], expected)
                    if expected in ('Fresh', 'Reference'):
                        self.assertEqual(actual['value' if expected == 'Fresh' else 'reference_value'], '6')
                    if source_age == -1:
                        self.assertEqual(row(run([s([c('p', 'PUMP', '2')])], quotes, refs=quotes, at=T+1), 'exchange:PUMP')['value'], '6')
                    self.assertEqual(skewed['evidence'], original)

    def test_common_fx_clock_skew_does_not_expand_seven_unpriced_groups_to_thirteen(self):
        parts = [c('usdt', 'USDT', '2')]
        quotes = [q('USDT', '1000')]
        for i in range(5):
            parts.append(c('priced'+str(i), 'FAKE'+str(i), '1'))
            quotes.append(q('FAKE'+str(i), '1', 'binance', 'USDT'))
        parts.extend(c('unpriced'+str(i), 'UNPRICED'+str(i), '1') for i in range(7))
        baseline = run([s(parts)], quotes)
        self.assertEqual(baseline['screen']['unvalued_groups'], 7)
        self.assertEqual(baseline['valuation']['known_subtotal'], '7000')
        quotes[0]['evidence']['received_at_ms'] -= 19
        original = copy.deepcopy(quotes[0]['evidence'])
        refreshed = run([s(parts)], quotes)
        self.assertEqual(refreshed['screen']['unvalued_groups'], 7)
        self.assertEqual(refreshed['valuation']['known_subtotal'], '7000')
        self.assertEqual(quotes[0]['evidence'], original)

    def test_all_unvalued_screen_keeps_the_actual_cpp_header_and_missing_figure(self):
        for references in ([], [q('BTC', '5', age=180_001)]):
            result = run([s([c('b', 'BTC', '1')])], refs=references)
            self.assertEqual(result['screen']['label'], '평가액')
            self.assertEqual(result['screen']['figure'], '미평가')
            self.assertFalse(result['screen']['complete'])

    def test_new_display_keeps_no_price_distinct_from_confirmed_zero_and_old_exports(self):
        connection = {'id': 'c1', 'environment': 'live', 'state': 'complete'}
        data = {'format': recalc.FORMAT, 'as_of_ms': T, 'connections': [connection],
                'snapshots': [s([c('unpriced', 'BTC', '1')])]}
        legacy = recalc.evaluate(recalc.Bundle.load(data))
        data['screen_presentation'] = 2
        updated = recalc.evaluate(recalc.Bundle.load(data))
        self.assertEqual(legacy['valuation'], updated['valuation'])
        self.assertEqual((legacy['screen']['label'], legacy['screen']['figure']), ('평가액', '미평가'))
        self.assertEqual((updated['screen']['label'], updated['screen']['figure']), ('확인된 자산 부분합', '—'))
        for cash in ('0', '12'):
            data['snapshots'] = [s([c('unpriced', 'BTC', '1'), c('cash', 'KRW', cash, kind='Cash')])]
            screen = recalc.evaluate(recalc.Bundle.load(data))['screen']
            self.assertEqual((screen['label'], screen['figure']), ('확인된 자산 부분합', cash))

    def test_original_time_wins_and_private_prices_cannot_value_another_account(self):
        latest_source = q('BTC', '102', age=2_000, source_age=2_000)
        delayed = q('BTC', '99', age=1_000, source_age=10_000)
        private = q('BTC', '999', 'account', age=500)
        for quotes in itertools.permutations([latest_source, delayed, private]):
            self.assertEqual(row(run([s([c('b', 'BTC', '1')])], quotes), 'exchange:BTC')['value'], '102')

    def test_direct_conflict_falls_back_to_independent_cross_and_ignores_old_conflicts(self):
        old = q('BTC', '1', age=5_000)
        clash = q('BTC', '2', age=5_000)
        newer = q('BTC', '3', age=1_000)
        for quotes in itertools.permutations([old, clash, newer]):
            self.assertEqual(row(run([s([c('b', 'BTC', '1')])], quotes), 'exchange:BTC')['value'], '3')
        cross = [q('BTC', '0.5', 'binance', 'USDT'), q('USDT', '1400')]
        result = run([s([c('b', 'BTC', '1')])], [old, clash] + cross)
        self.assertEqual(row(result, 'exchange:BTC')['value'], '700')

    def test_reference_expires_after_exactly_twenty_four_hours(self):
        for age, state in [(86_400_000, 'Reference'), (86_400_001, 'Unvalued')]:
            result = run([s([c('b', 'BTC', '1')])], refs=[q('BTC', '5', age=age)])
            self.assertEqual(row(result, 'exchange:BTC')['state'], state)

    def test_invalid_cross_leg_cannot_hide_behind_the_other_legs_time(self):
        for fault in ['future-source', 'future-receipt', 'no-url', 'no-source', 'no-receipt', 'wrong-scope']:
            coin, fx = q('BTC', '0.5', 'binance', 'USDT'), q('USDT', '1400')
            if fault == 'future-source': coin['evidence']['source_time_ms'] = T + 1
            if fault == 'future-receipt': coin['evidence']['received_at_ms'] = T + 1
            if fault == 'no-url': coin['evidence']['url'] = ''
            if fault == 'no-source': coin['evidence']['source'] = ''
            if fault == 'no-receipt': coin['evidence']['received_at_ms'] = 0
            if fault == 'wrong-scope': coin['evidence']['scope'] = 'public:binance:OTHERUSDT'
            with self.subTest(fault=fault):
                self.assertEqual(row(run([s([c('b', 'BTC', '1')])], [coin, fx]), 'exchange:BTC')['state'], 'Unvalued')

    def test_solana_multiplies_won_before_the_twenty_four_digit_division(self):
        mint = 'solana:' + recalc.USDC_MINT
        token = 'solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3'
        result = run([s([c('t', token, '1')], market='solana')],
                     [q(token, '1', 'jupiter', 'USD'), q(mint, '3', 'jupiter', 'USD'), q('USDC', '1000')])
        self.assertEqual(row(result, token)['value'], '333.333333333333333333333333')

    def test_reported_currency_is_required_and_incomplete_pnl_is_not_shown(self):
        stock = c('s', 'STOCK', '1', reported_value='100')
        self.assertEqual(row(run([s([stock])]), 'exchange:STOCK')['state'], 'Unvalued')
        pnl = c('e', 'KRW', '1', kind='Equity', provider_unrealized_pnl='1', provider_pnl_currency='KRW', evidence=ev(complete=False))
        self.assertIsNone(row(run([s([pnl])]), 'cash:KRW')['provider_pnl'])

    def test_duplicate_rows_are_preserved_but_duplicate_equity_is_not_counted(self):
        result = run([s([c('e1', 'KRW', '10', kind='Equity'), c('e2', 'KRW', '20', kind='Equity')])])
        self.assertEqual(result['valuation']['known_subtotal'], '10')
        result = run([s([c('a', 'KRW', '1'), c('a', 'KRW', '2')])])
        self.assertEqual(len(next(g for g in result['groups'] if g['identity'] == 'cash:KRW')['rows']), 2)

    def test_canonical_exclusion_preserves_quantity_and_does_not_hide_a_different_mint(self):
        asset = 'solana:' + recalc.USDC_MINT
        other = 'solana:SKHYhSjuRWHgikq8eRKbtBbpABgJSkd7ytQV14i9EQ3'
        bundle = recalc.Bundle.load({'format': recalc.FORMAT, 'as_of_ms': T,
            'connections': [{'id': 'c1', 'environment': 'mainnet', 'state': 'complete'}],
            'snapshots': [s([c('a', asset, '2'), c('b', other, '3')], market='solana')],
            'quotes': [], 'excluded_assets': [asset, 'SKHY']})
        result = recalc.evaluate(bundle)
        excluded = row(result, asset)
        self.assertEqual((excluded['state'], excluded['quantity'], excluded['excluded_by_user']), ('NotCounted', '2', True))
        self.assertEqual(next(g for g in result['groups'] if g['identity'] == asset)['quantity'], '2')
        self.assertEqual(row(result, other)['state'], 'Unvalued')
        self.assertEqual((result['screen']['excluded_groups'], result['screen']['label']), (1, '평가 대상 부분합'))
        bundle.excluded_assets.append(other)
        result = recalc.evaluate(bundle)
        self.assertTrue(result['valuation']['complete'])
        self.assertEqual((result['screen']['excluded_groups'], result['screen']['label'], result['screen']['figure']),
                         (2, '평가 대상 합계', '0'))

    def test_zero_presence_and_saved_scope_follow_the_cpp_rules(self):
        held = c('x', 'KRW', '1', reported_value='invalid')
        self.assertEqual(row(run([s([held])]), 'cash:KRW')['presence'], 'Held')
        bad_zero = c('z', 'KRW', '0', evidence=ev(complete=False))
        self.assertEqual(row(run([s([bad_zero])]), 'cash:KRW')['presence'], 'NeedsCheck')
        data = {'format': recalc.FORMAT, 'as_of_ms': T,
                'connections': [{'id':'c1','environment':'live','state':'not_read'}],
                'snapshots':[s([c('cash','KRW','1')])], 'quotes':[]}
        saved = recalc.recorded(recalc.Bundle.load(data), T)
        self.assertTrue(saved['screen']['complete'])

    def test_sum_precision_overflow_does_not_reuse_the_previous_subtotal(self):
        result = run([s([c('large', 'KRW', '9' * 80), c('one', 'KRW', '1')])])
        self.assertFalse(result['valuation']['complete'])
        self.assertIsNone(result['valuation']['known_subtotal'])
        self.assertEqual(result['screen']['figure'], '자료 부족')
        self.assertEqual([r['state'] for g in result['groups'] for r in g['rows']], ['Fresh', 'Fresh'])
        result = run([s([c('small', 'BTC', '0.0001')])],
                     [q('BTC', '9' * 80, 'binance', 'USDT'), q('USDT', '1000')])
        self.assertEqual(row(result, 'exchange:BTC')['state'], 'Unvalued')

    def test_signed_sum_limit_applies_after_cancellation_for_every_input_order(self):
        maximum = '9' * 80
        components = [c('large', 'KRW', maximum, kind='Cash'),
                      c('one', 'KRW', '1', kind='Cash'),
                      c('debt', 'KRW', '1', kind='Liability')]
        for permutation in itertools.permutations(components):
            with self.subTest(order=[part['id'] for part in permutation]):
                result = run([s(list(permutation))])
                self.assertTrue(result['valuation']['complete'])
                self.assertEqual(result['valuation']['net_assets'], maximum)
                self.assertEqual(result['valuation']['known_subtotal'], maximum)
                group = next(g for g in result['groups'] if g['identity'] == 'cash:KRW')
                self.assertEqual((group['fresh_total'], group['quantity']), (maximum, maximum))
                self.assertTrue(all(r['state'] == 'Fresh' for r in group['rows']))
                reference_parts = [dict(part, asset='BTC') for part in permutation]
                reference = run([s(reference_parts)], refs=[q('BTC', '1', age=180_001)])
                self.assertEqual(reference['screen']['reference_total'], maximum)
        for permutation in itertools.permutations(components[:2]):
            result = run([s(list(permutation))])
            self.assertFalse(result['valuation']['complete'])
            self.assertIsNone(result['valuation']['known_subtotal'])
            group = next(g for g in result['groups'] if g['identity'] == 'cash:KRW')
            self.assertIsNone(group['fresh_total'])
            self.assertIsNone(group['quantity'])
            self.assertTrue(all(r['state'] == 'Fresh' for r in group['rows']))
        reference_parts = [dict(part, asset='BTC') for part in components[:2]]
        reference = run([s(reference_parts)], refs=[q('BTC', '1', age=180_001)])
        self.assertIsNone(reference['screen']['reference_total'])

    def test_stale_direct_conflict_can_use_independent_reference_legs(self):
        first, second = q('BTC', '10', age=500_000), q('BTC', '11', age=500_000)
        result = run([s([c('b', 'BTC', '1')])], refs=[first, second,
                     q('BTC', '0.5', 'binance', 'USDT', age=400_000), q('USDT', '1400', age=400_000)])
        self.assertEqual((row(result, 'exchange:BTC')['state'], row(result, 'exchange:BTC')['reference_value']),
                         ('Reference', '700'))

    def test_source_time_zero_uses_public_receipt_and_reported_usd_requires_a_price(self):
        quote = q('BTC', '10')
        quote['evidence']['source_time_ms'] = 0
        self.assertEqual(row(run([s([c('b', 'BTC', '1')])], [quote]), 'exchange:BTC')['value'], '10')
        reported = c('usd', 'STOCK', '99', reported_value='2', reported_value_currency='USD')
        self.assertEqual(row(run([s([reported])]), 'exchange:STOCK')['state'], 'Unvalued')
        self.assertEqual(row(run([s([reported])], [q('USD', '1300')]), 'exchange:STOCK')['value'], '2600')

    def test_recorded_scope_filters_unrelated_private_quotes_and_marks_missing_connections(self):
        foreign = q('BTC', '1', 'account')
        foreign['evidence']['scope'], foreign['evidence']['received_at_ms'] = 'foreign', T + 1
        data = {'format': recalc.FORMAT, 'as_of_ms': T,
                'connections': [{'id':'c1','environment':'live','state':'not_read'},
                                {'id':'missing','environment':'live','state':'complete'}],
                'snapshots':[s([c('cash','KRW','1')])], 'quotes':[foreign]}
        saved = recalc.recorded(recalc.Bundle.load(data), T)
        self.assertIsNotNone(saved)
        self.assertFalse(saved['screen']['complete'])
        self.assertEqual(saved['screen']['label'], '마지막 확인 평가액')


def row(result, identity, component=None):
    group = next(g for g in result["groups"] if g["identity"] == identity)
    return group["rows"][0] if component is None else next(r for r in group["rows"] if r["component_id"] == component)


class Freshness(unittest.TestCase):
    def test_price_exactly_180_seconds_old_is_fresh_one_ms_more_is_not(self):  # §1
        ok = run([s([c("b", "BTC", "2")])], [q("BTC", "100", age=180_000)])
        self.assertEqual(row(ok, "exchange:BTC")["state"], "Fresh")
        aged = q("BTC", "100", age=180_001)
        late = run([s([c("b", "BTC", "2")])], [aged], refs=[aged])
        self.assertEqual(row(late, "exchange:BTC")["state"], "Reference")
        self.assertEqual(row(late, "exchange:BTC")["reference_value"], "200")
        self.assertEqual(late["valuation"]["known_subtotal"], "0")

    def test_missing_source_time_is_judged_by_receipt(self):  # §1
        result = run([s([c("k", "KRW", "5", kind="Cash")])])
        self.assertEqual(row(result, "cash:KRW")["state"], "Fresh")

    def test_future_source_time_is_not_fresh(self):  # §1
        result = run([s([c("b", "BTC", "1")])], [q("BTC", "100", age=1_000, source_age=-5_000)])
        self.assertEqual(row(result, "exchange:BTC")["state"], "Unvalued")


class Balances(unittest.TestCase):
    def test_stale_row_evidence_makes_rows_unvalued_not_reference(self):  # §2
        aged = q("BTC", "100", age=200_000)
        result = run([s([c("b", "BTC", "1")], age=180_001)], [q("BTC", "100")], refs=[aged])
        self.assertEqual(row(result, "exchange:BTC")["state"], "Unvalued")
        self.assertFalse(result["valuation"]["complete"])

    def test_incomplete_account_keeps_fresh_rows_in_the_partial_total(self):  # §2, review R4
        result = run([s([c("cash", "KRW", "1000", kind="Cash")], complete=False)])
        cash = row(result, "cash:KRW")
        self.assertEqual((result["valuation"]["complete"], result["valuation"]["known_subtotal"], cash["state"], cash["value"]),
                         (False, "1000", "Fresh", "1000"))
        self.assertIsNone(result["valuation"]["net_assets"])
        stale = {**c("cash", "KRW", "1000", kind="Cash"), "evidence": ev(age=180_001)}
        result = run([s([stale], complete=False)])
        self.assertEqual((row(result, "cash:KRW")["state"], result["valuation"]["known_subtotal"]), ("Unvalued", "0"))
        account_only = s([{**c("cash", "KRW", "1000", kind="Cash"), "evidence": ev()}], age=180_001)
        result = run([account_only])
        self.assertEqual((result["valuation"]["complete"], row(result, "cash:KRW")["value"]), (False, "1000"))

    def test_duplicate_scope_and_row_ids_break_completeness(self):  # §4
        twice = run([s([c("b", "BTC", "1")]), s([c("b", "BTC", "1")], conn="c2")], [q("BTC", "1")])
        self.assertFalse(twice["valuation"]["complete"])
        rows = run([s([c("b", "BTC", "1"), c("b", "BTC", "1")])], [q("BTC", "1")])
        self.assertFalse(rows["valuation"]["complete"])


class Routes(unittest.TestCase):
    def test_krw_and_zero_need_no_price(self):  # §3.1
        result = run([s([c("k", "KRW", "1234.5", kind="Cash"), c("z", "ZZZ", "0")])])
        self.assertEqual(row(result, "cash:KRW")["value"], "1234.5")
        self.assertEqual(row(result, "exchange:ZZZ")["state"], "Fresh")
        self.assertEqual(row(result, "exchange:ZZZ")["value"], "0")
        self.assertTrue(result["valuation"]["complete"])

    def test_reported_value_replaces_quantity_times_price(self):  # §3.2
        stock = c("005930", "005930", "10", reported_value="712000", reported_value_currency="KRW")
        result = run([s([stock], market="domestic_equity")])
        self.assertEqual(row(result, "stock:KR:005930")["value"], "712000")

    def test_latest_original_time_wins_and_a_source_tie_with_two_prices_is_unvalued(self):
        result = run([s([c("b", "BTC", "1")])], [q("BTC", "100", age=1_000, source_age=1_000), q("BTC", "101", age=500, source_age=2_000)])
        self.assertEqual(row(result, "exchange:BTC")["value"], "100")
        tie = run([s([c("b", "BTC", "1")])], [q("BTC", "100", age=2_000), q("BTC", "101", age=2_000)])
        self.assertEqual(row(tie, "exchange:BTC")["state"], "Unvalued")

    def test_binance_cross_only_when_no_fresh_direct_price(self):  # §3.4
        quotes = [q("BTC", "90", age=200_000), q("BTC", "0.5", "binance", "USDT", age=3_000),
                  q("USDT", "1400", age=1_000)]
        result = run([s([c("b", "BTC", "2")])], quotes)
        btc = row(result, "exchange:BTC")
        self.assertEqual(btc["value"], "1400")
        self.assertEqual(btc["price"]["route"], "binance-cross")
        self.assertEqual(btc["price"]["evidence"]["received_at_ms"], T - 3_000)
        direct = run([s([c("b", "BTC", "2")])], quotes + [q("BTC", "800")])
        self.assertEqual(row(direct, "exchange:BTC")["value"], "1600")

    def test_an_aged_cross_result_is_never_counted_and_becomes_a_reference(self):  # §3.4, §5
        quotes = [q("BTC", "0.5", "binance", "USDT", age=181_000), q("USDT", "1400")]
        result = run([s([c("b", "BTC", "1")])], quotes)
        self.assertEqual(row(result, "exchange:BTC")["state"], "Reference")
        self.assertEqual(row(result, "exchange:BTC")["reference_value"], "700")
        self.assertEqual(result["valuation"]["known_subtotal"], "0")

    def test_usdt_never_takes_a_cross_route(self):  # §3.4
        quotes = [q("USDT", "1", "binance", "USDT"), q("USDT", "1400", age=200_000)]
        self.assertEqual(row(run([s([c("u", "USDT", "1")])], quotes), "exchange:USDT")["state"], "Unvalued")

    def test_solana_cross_truncates_the_division_at_24_places(self):  # §3.4
        mint = "solana:Mint111"
        quotes = [q(mint, "1", "jupiter", "USD"), q("solana:" + recalc.USDC_MINT, "3", "jupiter", "USD"),
                  q("USDC", "1500")]
        result = run([s([c("t", mint, "3")], market="solana")], quotes)
        token = row(result, mint)
        self.assertEqual(token["price"]["price"], "500")
        self.assertEqual(token["value"], "1500")

    def test_thin_upbit_usdc_yields_to_a_fresher_binance_leg(self):  # §3.4
        mint = "solana:Mint111"
        base = [q(mint, "140", "jupiter", "USD"), q("solana:" + recalc.USDC_MINT, "1", "jupiter", "USD"),
                q("USDC", "1.001", "binance", "USDT", age=1_000), q("USDT", "1336")]
        stale = q("USDC", "1338", age=45_000, source_age=426_000)
        token = row(run([s([c("t", mint, "2")], market="solana")], base + [stale]), mint)
        self.assertEqual((token["state"], token["value"]), ("Fresh", "374454.08"))
        same_time = q("USDC", "1340", age=1_000)
        direct = row(run([s([c("t", mint, "2")], market="solana")], base + [same_time]), mint)
        self.assertEqual(direct["value"], "375200")

    def test_solana_legs_need_an_original_time(self):  # §3.4
        mint = "solana:Mint111"
        quotes = [q(mint, "1", "jupiter", "USD"), q("solana:" + recalc.USDC_MINT, "1", "jupiter", "USD"),
                  {**q("USDC", "1500"), "evidence": ev(1_000, None, scope="public:upbit")}]
        self.assertEqual(row(run([s([c("t", mint, "1")], market="solana")], quotes), mint)["state"], "Unvalued")


class Totals(unittest.TestCase):
    def test_kinds_liabilities_and_equity_groups(self):  # §3, §4
        components = [c("cash", "KRW", "1000", kind="Cash"), c("loan", "KRW", "300", kind="Liability"),
                      c("bp", "KRW", "999", kind="BuyingPower"),
                      c("eq", "USDT", "10", kind="Equity", group="futures", provider_unrealized_pnl="0"),
                      c("held", "USDT", "4", group="futures"),
                      c("pos", "BTCUSDT", "0.1", kind="PositionNotional", group="positions")]
        result = run([s(components)], [q("USDT", "1400")])
        self.assertEqual(result["valuation"]["known_subtotal"], "14700")
        self.assertTrue(result["valuation"]["complete"])
        states = {r["component_id"]: r["state"] for g in result["groups"] for r in g["rows"]}
        self.assertEqual(states, {"cash": "Fresh", "loan": "Fresh", "bp": "NotCounted", "eq": "Fresh",
                                  "held": "NotCounted", "pos": "NotCounted"})

    def test_screen_needs_every_live_connection_complete(self):  # §4
        snapshots = [s([c("k", "KRW", "1", kind="Cash")])]
        connections = [{"id": "c1", "environment": "live", "state": "complete"},
                       {"id": "c2", "environment": "live", "state": "failed"},
                       {"id": "c3", "environment": "demo", "state": "failed"}]
        result = run(snapshots, connections=connections)
        self.assertTrue(result["valuation"]["complete"])
        self.assertEqual(result["screen"]["label"], "평가액")
        connections[1]["state"] = "complete"
        self.assertEqual(run(snapshots, connections=connections)["screen"]["label"], "순자산")

    def test_no_fresh_value_reads_unvalued(self):  # §4
        result = run([s([c("b", "BTC", "1")])])
        self.assertEqual((result["screen"]["label"], result["screen"]["figure"]), ("평가액", "미평가"))

    def test_reference_within_twenty_four_hours_stays_out_of_totals(self):
        old = q("BTC", "100", age=10 * 3_600_000)
        result = run([s([c("b", "BTC", "1"), c("k", "KRW", "5", kind="Cash")])], refs=[old])
        self.assertEqual(row(result, "exchange:BTC")["state"], "Reference")
        self.assertEqual(result["screen"]["reference_total"], "100")
        self.assertEqual(result["valuation"]["known_subtotal"], "5")


class Presentation(unittest.TestCase):
    def test_wallet_and_exchange_sol_stay_apart_while_krw_merges(self):  # §6
        result = run([s([c("sol", "SOL", "1"), c("k", "KRW", "1", kind="Cash")]),
                      s([c("solana:native", "SOL", "2")], scope="w", market="solana", conn="c2"),
                      s([c("cash", "KRW", "2", kind="Cash")], scope="b", market="domestic_equity", conn="c3")],
                     [q("SOL", "10")])
        identities = {g["identity"]: g["quantity"] for g in result["groups"]}
        self.assertEqual(identities["exchange:SOL"], "1")
        self.assertEqual(identities["solana:native"], "2")
        self.assertEqual(identities["cash:KRW"], "3")

    def test_group_quantity_and_order(self):  # §6
        result = run([s([c("a", "AAA", "1"), c("b", "BBB", "1"), c("u", "UUU", "1"), c("z", "ZZZ", "0"),
                         c("l", "AAA", "0.4", kind="Liability")])],
                     [q("AAA", "10"), q("BBB", "50")])
        self.assertEqual([g["identity"] for g in result["groups"]],
                         ["exchange:BBB", "exchange:AAA", "exchange:UUU", "exchange:ZZZ"])
        self.assertEqual(next(g for g in result["groups"] if g["identity"] == "exchange:AAA")["quantity"], "0.6")

    def test_shares_round_half_up_in_tenths(self):  # §7
        self.assertEqual(recalc.share(Decimal(1), Decimal(3)), "33.3%")
        self.assertEqual(recalc.share(Decimal(2), Decimal(3)), "66.7%")
        self.assertEqual(recalc.share(Decimal("0.5"), Decimal(1000)), "0.1%")
        self.assertEqual(recalc.share(Decimal("0.49"), Decimal(1000)), "<0.1%")

    def test_allocation_bar_keeps_four_and_rest(self):  # §7
        components = [c(f"x{i}", f"A{i}", "1") for i in range(6)] + [c("l", "KRW", "5", kind="Liability")]
        result = run([s(components)], [q(f"A{i}", str(10 * (i + 1))) for i in range(6)])
        bar = result["allocation"]["bar"]
        self.assertEqual([b["identity"] for b in bar][-1], "기타")
        self.assertEqual(len(bar), 5)
        self.assertEqual(bar[-1]["value"], "30")
        self.assertEqual(result["allocation"]["total"], "210")

    def test_krw_rounds_half_up_on_magnitude(self):  # §8
        cases = {"1234.5": "1,235", "1234.49": "1,234", "-0.5": "-1", "-0.4": "0", "999999.5": "1,000,000"}
        for value, shown in cases.items():
            self.assertEqual(recalc.krw(Decimal(value)), shown)
        big = "9" * 79 + ".5"
        self.assertEqual(recalc.krw(Decimal(big)).replace(",", ""), "1" + "0" * 79)

    def test_dust_hides_only_fully_fresh_groups_below_one_usdt(self):  # §9
        components = [c("a", "AAA", "1"), c("b", "BBB", "1"), c("c", "CCC", "1"), c("d", "DDD", "x")]
        quotes = [q("USDT", "1400"), q("AAA", "100"), q("BBB", "2000"), q("DDD", "1")]
        result = run([s(components)], quotes)
        self.assertEqual(result["dust"]["hidden"], ["exchange:AAA"])
        self.assertEqual(result["dust"]["hidden_total"], "100")
        self.assertEqual(result["valuation"]["known_subtotal"], "2100")
        self.assertIsNone(run([s(components)], quotes[1:])["dust"]["threshold"])

    def test_zero_futures_equity_is_zero_only_with_explicit_zero_pnl(self):  # §9
        zero = c("eq", "USDT", "0", kind="Equity", group="futures", provider_unrealized_pnl="0")
        unknown = c("eq", "USDT", "0", kind="Equity", group="futures")
        self.assertEqual(row(run([s([zero])]), "exchange:USDT")["presence"], "Zero")
        self.assertEqual(row(run([s([unknown])]), "exchange:USDT")["presence"], "NeedsCheck")

    def test_hide_unvalued_takes_only_wholly_unvalued_groups(self):  # §10
        result = run([s([c("a", "AAA", "1"), c("b", "BBB", "1")]),
                      s([c("b2", "BBB", "1")], scope="other", conn="c2")], [q("BBB", "1")],
                     refs=[q("AAA", "1", age=500_000)])
        self.assertEqual(result["hide_unvalued"], [])
        self.assertEqual(run([s([c("a", "AAA", "1")])])["hide_unvalued"], ["exchange:AAA"])

    def test_provider_pnl_needs_matching_scope_and_fresh_times(self):  # §11
        pnl = c("eq", "USDT", "1", kind="Equity", group="f", provider_unrealized_pnl="-2.5", provider_pnl_currency="USDT")
        self.assertEqual(row(run([s([pnl])], [q("USDT", "1")]), "exchange:USDT")["provider_pnl"], "-2.5")
        other = {**pnl, "evidence": ev(scope="elsewhere")}
        self.assertIsNone(row(run([s([other])], [q("USDT", "1")]), "exchange:USDT")["provider_pnl"])

    def test_recorded_view_uses_the_latest_saved_time_and_rejects_the_future(self):  # §12
        data = {"format": recalc.FORMAT, "as_of_ms": T, "connections": [{"id": "c1", "environment": "live", "state": "complete"}],
                "snapshots": [s([c("b", "BTC", "1")], age=600_000)], "quotes": [q("BTC", "100", age=500_000)]}
        saved = recalc.recorded(recalc.Bundle.load(data), T)
        self.assertEqual(saved["as_of_ms"], T - 500_000)
        self.assertEqual(saved["valuation"]["known_subtotal"], "100")
        data["quotes"][0]["evidence"]["received_at_ms"] = T + 1
        self.assertIsNone(recalc.recorded(recalc.Bundle.load(data), T))


class Properties(unittest.TestCase):
    def random_case(self, rng):
        assets = ["AAA", "BBB", "CCC", "USDT", "KRW"]
        snapshots = []
        for n in range(rng.randint(1, 3)):
            parts = []
            for i in range(rng.randint(1, 6)):
                asset = rng.choice(assets)
                quantity = rng.choice(["0", str(rng.randint(0, 10**6)), f"{rng.randint(0, 999)}.{rng.randint(0, 10**18):018d}"])
                kind = "Cash" if asset == "KRW" else rng.choice(["Holding", "Holding", "Liability", "BuyingPower"])
                parts.append(c(f"r{i}", asset, quantity, kind=kind))
            snapshots.append(s(parts, scope=f"s{n}", conn=f"c{n}", age=rng.choice([1_000, 1_000, 190_000])))
        quotes = [q(a, f"{rng.randint(1, 10**7)}.{rng.randint(0, 99):02d}", age=rng.choice([1_000, 179_000, 181_000]))
                  for a in assets[:-1] if rng.random() < 0.8]
        quotes += [q(a, str(rng.randint(1, 900)), "binance", "USDT", age=rng.choice([1_000, 200_000]))
                   for a in assets[:3] if rng.random() < 0.5]
        return snapshots, quotes

    def test_totals_are_exact_sums_and_independent_of_input_order(self):
        rng = random.Random(20261020)
        for _ in range(300):
            snapshots, quotes = self.random_case(rng)
            result = run(snapshots, quotes, refs=quotes)
            fresh = Decimal(0)  # exact sum; Python's default context would round at 28 digits
            for g in result["groups"]:
                for r in g["rows"]:
                    if r["state"] == "Fresh":
                        fresh = recalc.add(fresh, Decimal(r["value"]))
            self.assertEqual(Decimal(result["valuation"]["known_subtotal"]), fresh)
            if result["valuation"]["complete"]:
                self.assertEqual(result["valuation"]["net_assets"], result["valuation"]["known_subtotal"])
            hidden = Decimal(result["dust"]["hidden_total"])
            self.assertEqual(recalc.add(Decimal(result["dust"]["subtotal_after_hiding"]), hidden), fresh)
            shuffled_snapshots, shuffled_quotes = copy.deepcopy(snapshots), copy.deepcopy(quotes)
            rng.shuffle(shuffled_snapshots)
            rng.shuffle(shuffled_quotes)
            again = run(shuffled_snapshots, shuffled_quotes, refs=shuffled_quotes)
            self.assertEqual(again["valuation"], result["valuation"])
            self.assertEqual({g["identity"]: g["fresh_total"] for g in again["groups"]},
                             {g["identity"]: g["fresh_total"] for g in result["groups"]})


class Example(unittest.TestCase):
    def test_example_bundle_runs(self):
        bundle = Path(__file__).with_name("example-bundle.json")
        self.assertEqual(recalc.main([str(bundle)]), 0)


if __name__ == "__main__":
    unittest.main()
