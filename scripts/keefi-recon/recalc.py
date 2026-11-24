#!/usr/bin/env python3
"""Recompute what KEEFI should show from one input bundle.

The original build-47 implementation was written from its policy document.
G7 changes were checked against both that document and C++ behavior; this is
a separate runtime implementation, not an author-independent audit. It does
not load KEEFI sources or libraries while evaluating a supplied bundle.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from legacy_formats import normalize_bundle
from decimal import (Context, Decimal, DivisionByZero, Inexact, InvalidOperation,
                     Overflow, ROUND_DOWN, ROUND_HALF_UP)

FORMAT = "keefi-valuation-bundle/1"
FRESH_MS = 180_000  # §1
REFERENCE_MAX_AGE_MS = 86_400_000
PUBLIC_SOURCE_RECEIPT_SKEW_MS = 2_000
USDC_MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"  # public Solana USDC mint (G6-BUILD47)
VENUES = {"upbit", "binance", "jupiter", "account"}
BASE_KINDS = {"Holding", "Cash", "Liability"}
KINDS = BASE_KINDS | {"Equity", "BuyingPower", "Withdrawable", "PositionNotional",
                      "UnrealizedPnl", "ProviderPnl"}
LIVE = {"live", "mainnet"}

# Additions and products must be exact; a trap here means a policy limit was missed.
EXACT = Context(prec=6000, traps=[InvalidOperation, Inexact, Overflow, DivisionByZero])
TRUNCATE = Context(prec=6000, rounding=ROUND_DOWN, traps=[InvalidOperation, Overflow, DivisionByZero])
AMOUNT = re.compile(r"-?[0-9]{1,80}(?:\.[0-9]{1,80})?")  # §0

POLICY_DECISIONS = {
    "A1": "공개 다리는 정확한 scope, 양수 가격, complete/source/URL, 양의 원본·수신 시각과 as-of 경계를 각각 검증한다.",
    "A2": "truncate24(Mint USD × USDC KRW ÷ USDC Mint USD)를 계산한 뒤 수량을 곱한다.",
    "A3": "합성 근거의 원본·수신 시각은 모든 실제 3개 또는 4개 다리 중 최소다.",
    "A4": "직접 가격은 원본(없으면 수신) 시각 우선. 최신 동률의 정규화 가격이 충돌하면 독립 교차·참고 경로를 시도한다.",
    "A5": "직접 참고 가격 충돌은 독립 교차 참고 경로를 막지 않는다. 참고 나이는 최대 24시간이다.",
    "A6": "정수·소수 각각 80자리. 부호 상쇄를 마친 최종 합계에 상한을 적용하고, 초과 시 합계만 무효로 하며 개별 유효 행은 보존한다.",
    "A7": "보고 통화는 명시해야 하며 그 통화의 유효한 공개 환산 경로만 사용한다.",
    "A8": "업체 손익은 complete/source/scope/통화와 최신시각을 모두 검증하고 합계에 가산하지 않는다.",
    "A9": "유효 비0가 하나라도 있으면 보유. 나머지 불확실 값·불완전0은 확인 필요. 선물0은 완전조회·명시0손익·Fresh가 필요하다.",
    "A10": "최신 값 그룹 평가액 내림차순, 나머지, 확인된0 순서이며 동률은 canonical identity 순이다.",
    "A11": "직접·참고 가격은 공개 scope만 사용하며 다른 계좌의 사적 가격을 공유하지 않는다.",
    "A12": "중복 계좌 범위는 뒤 스냅숏 제외, 중복 행은 미평가로 보존. 동일 그룹·자산 equity의 이중 가산을 막는다.",
    "A13": "저장 연결·scope만 고르고 상태를 재구성하며 최대 저장시각으로 평가한다. 표시 허가는 앱의 동일세션·검증scope조건을 별도로 따른다.",
}
ASSUMPTIONS = {}  # Compatibility field: A1–A13 are decisions confirmed on 2026-10-21.


def parse(value) -> Decimal | None:
    return Decimal(value) if isinstance(value, str) and AMOUNT.fullmatch(value) else None


def positive(value) -> bool:
    number = parse(value)
    return number is not None and number > 0


def text(number: Decimal) -> str:
    if number == 0:
        return "0"
    out = format(number, "f")
    return out.rstrip("0").rstrip(".") if "." in out else out


def fits(number: Decimal) -> bool:  # §0, A6
    whole, _, fraction = text(number).lstrip("-").partition(".")
    return 1 <= len(whole) <= 80 and len(fraction) <= 80


def add(a: Decimal, b: Decimal) -> Decimal:
    return EXACT.add(a, b)


def mul(a: Decimal, b: Decimal) -> Decimal:
    return EXACT.multiply(a, b)


def bounded_add(a: Decimal | None, b: Decimal | None) -> Decimal | None:
    if a is None or b is None:
        return None
    total = add(a, b)
    return total if fits(total) else None


def bounded_sum(values) -> Decimal | None:
    """Apply the amount limit to the exact final sum, after signed cancellation."""
    total = Decimal(0)
    for value in values:
        if value is None:
            return None
        total = add(total, value)
    return total if fits(total) else None


def grouped(number_text: str) -> str:
    negative = number_text.startswith("-")
    whole, dot, fraction = number_text.lstrip("-").partition(".")
    groups = []
    while len(whole) > 3:
        groups.insert(0, whole[-3:])
        whole = whole[:-3]
    groups.insert(0, whole)
    return ("-" if negative else "") + ",".join(groups) + (dot + fraction if dot else "")


def krw(number: Decimal) -> str:  # §8: half-up on the magnitude, whole won
    rounded = number.copy_abs().quantize(Decimal(1), rounding=ROUND_HALF_UP, context=TRUNCATE)
    return grouped(("-" if number < 0 and rounded != 0 else "") + text(rounded))


# ---------------------------------------------------------------- inputs

@dataclass
class Evidence:
    source: str = ""
    url: str = ""
    scope: str = ""
    source_time_ms: int = 0
    received_at_ms: int = 0
    complete: bool = False

    @classmethod
    def load(cls, o: dict | None) -> "Evidence":
        o = o or {}
        return cls(o.get("source", ""), o.get("url", ""), o.get("scope", ""),
                   int(o.get("source_time_ms", 0)), int(o.get("received_at_ms", 0)),
                   bool(o.get("complete", False)))

    def as_dict(self) -> dict:
        return self.__dict__.copy()


def within(t: int, at: int, age: int = FRESH_MS) -> bool:
    return 0 < t <= at and at - t <= age


def fresh(e: Evidence, at: int, age: int = FRESH_MS) -> bool:  # §1
    return (e.complete and bool(e.source) and bool(e.scope) and within(e.received_at_ms, at, age)
            and (e.source_time_ms == 0 or within(e.source_time_ms, at, age)))


@dataclass
class Quote:
    base: str
    quote: str
    price: str
    venue: str
    evidence: Evidence

    @classmethod
    def load(cls, o: dict) -> "Quote":
        if o.get("venue") not in VENUES:
            raise ValueError(f"unknown venue: {o.get('venue')!r}")
        return cls(o["base"], o["quote"], o["price"], o["venue"], Evidence.load(o.get("evidence")))

    def leg(self) -> dict:
        return {"venue": self.venue, "pair": f"{self.base}/{self.quote}", "price": self.price,
                "source_time_ms": self.evidence.source_time_ms,
                "received_at_ms": self.evidence.received_at_ms, "evidence": self.evidence.as_dict()}


@dataclass
class Component:
    id: str
    asset: str
    kind: str
    quantity: str
    valuation_group: str
    evidence: Evidence
    reported_value: str = ""
    reported_value_currency: str = ""
    provider_unrealized_pnl: str = ""
    provider_pnl_currency: str = ""


@dataclass
class Snapshot:
    account_scope: str
    connection_id: str
    provider: str
    environment: str
    market: str
    time_ms: int
    balance_complete: bool
    evidence: Evidence
    components: list[Component]

    @classmethod
    def load(cls, o: dict) -> "Snapshot":
        evidence = Evidence.load(o.get("evidence"))
        parts = []
        for c in o.get("components", []):
            if c.get("kind") not in KINDS:
                raise ValueError(f"unknown component kind: {c.get('kind')!r}")
            parts.append(Component(
                c.get("id", ""), c.get("asset", ""), c["kind"], c.get("quantity", ""),
                c.get("valuation_group", ""),
                Evidence.load(c["evidence"]) if "evidence" in c else evidence,
                c.get("reported_value", ""), c.get("reported_value_currency", ""),
                c.get("provider_unrealized_pnl", ""), c.get("provider_pnl_currency", "")))
        return cls(o.get("account_scope", ""), o.get("connection_id", ""), o.get("provider", ""),
                   o.get("environment", ""), o.get("market", ""), int(o.get("time_ms", 0)),
                   bool(o.get("balance_complete", False)), evidence, parts)


@dataclass
class Bundle:
    as_of_ms: int
    connections: list[dict]
    snapshots: list[Snapshot]
    quotes: list[Quote]
    references: list[Quote]
    loading: bool = False
    excluded_assets: list[str] = field(default_factory=list)
    screen_presentation: int = 1

    @classmethod
    def load(cls, o: dict) -> "Bundle":
        o = normalize_bundle(o)
        if o.get("format") != FORMAT:
            raise ValueError(f"expected format {FORMAT}")
        presentation = o.get("screen_presentation", 1)  # Existing exports used the original labels.
        if type(presentation) is not int or presentation not in (1, 2):
            raise ValueError("unknown screen_presentation")
        return cls(int(o["as_of_ms"]), list(o.get("connections", [])),
                   [Snapshot.load(s) for s in o.get("snapshots", [])],
                   [Quote.load(q) for q in o.get("quotes", [])],
                   [Quote.load(q) for q in o.get("reference_quotes", [])],
                   bool(o.get("loading", False)), list(o.get("excluded_assets", [])), presentation)


# ---------------------------------------------------------------- prices (§3, §5)

CONFLICT = object()


def latest(candidates: list[Quote], key):
    """The candidate with the largest key, CONFLICT when that key carries two prices."""
    if not candidates:
        return None
    top = max(key(q) for q in candidates)
    tied = [q for q in candidates if key(q) == top]
    return CONFLICT if len({parse(q.price) for q in tied}) > 1 else tied[0]


def by_source(q: Quote) -> int:
    return q.evidence.source_time_ms or q.evidence.received_at_ms


def by_receipt(q: Quote) -> int:
    return q.evidence.received_at_ms


def public_quote(q: Quote, at: int) -> bool:
    e = q.evidence
    return (positive(q.price) and e.complete and bool(e.source) and bool(e.url)
            and e.scope.startswith("public:") and 0 <= e.source_time_ms <= at
            and 0 < e.received_at_ms <= at
            and e.source_time_ms - e.received_at_ms <= PUBLIC_SOURCE_RECEIPT_SKEW_MS)


def reference_age(e: Evidence, at: int) -> bool:
    original = e.source_time_ms or e.received_at_ms
    return within(original, at, REFERENCE_MAX_AGE_MS) and within(e.received_at_ms, at, REFERENCE_MAX_AGE_MS)


def leg_scope(venue: str, base: str, counter: str) -> str:
    if venue == "jupiter":
        return "public:jupiter:price:" + base.removeprefix("solana:")
    if venue == "upbit":
        return "public:upbit:" + counter + "-" + base
    return "public:binance:" + base + counter


def leg(quotes, venue, base, counter, at):
    pick = latest([q for q in quotes if q.venue == venue and q.base == base and q.quote == counter
                   and public_quote(q, at) and q.evidence.source_time_ms > 0
                   and q.evidence.scope == leg_scope(venue, base, counter)], by_source)
    return None if pick is CONFLICT else pick


@dataclass
class Price:
    price: Decimal
    evidence: Evidence
    route: str
    legs: list[dict]


def cross(asset: str, quotes: list[Quote], at: int) -> Price | None:
    if asset in ("KRW", "USDT"):
        return None
    if asset.startswith("solana:"):
        token = leg(quotes, "jupiter", asset, "USD", at)
        usdc = leg(quotes, "jupiter", "solana:" + USDC_MINT, "USD", at)
        if token is None or usdc is None:
            return None
        # §3.4: the USDC→KRW leg is Upbit KRW-USDC unless Binance USDC/USDT × Upbit
        # KRW-USDT has a later original time.
        upbit = leg(quotes, "upbit", "USDC", "KRW", at)
        binance = cross("USDC", quotes, at)
        if upbit is not None and (binance is None or upbit.evidence.source_time_ms >= binance.evidence.source_time_ms):
            won_price, won_legs = parse(upbit.price), [upbit]
        elif binance is not None:
            won_price, won_legs = binance.price, None
        else:
            return None
        numerator = mul(parse(token.price), won_price)
        if not fits(numerator):
            return None
        price = TRUNCATE.divide(numerator, parse(usdc.price)).quantize(
            Decimal("1e-24"), rounding=ROUND_DOWN, context=TRUNCATE)
        route = "solana-cross"
        if won_legs is None:
            evidence = Evidence(route, " × ".join([token.evidence.url, usdc.evidence.url, binance.evidence.url]), "derived:" + route,
                                min(token.evidence.source_time_ms, usdc.evidence.source_time_ms, binance.evidence.source_time_ms),
                                min(token.evidence.received_at_ms, usdc.evidence.received_at_ms, binance.evidence.received_at_ms), True)
            return Price(price, evidence, route, [token.leg(), usdc.leg()] + binance.legs) if price > 0 and fits(price) else None
        legs = [token, usdc] + won_legs
    else:
        coin = leg(quotes, "binance", asset, "USDT", at)
        fx = leg(quotes, "upbit", "USDT", "KRW", at)
        legs = [coin, fx]
        if None in legs:
            return None
        price, route = mul(parse(coin.price), parse(fx.price)), "binance-cross"
    if price <= 0 or not fits(price):
        return None
    evidence = Evidence(route, " × ".join(q.evidence.url for q in legs), "derived:" + route,  # A3
                        min(q.evidence.source_time_ms for q in legs),
                        min(q.evidence.received_at_ms for q in legs), True)
    return Price(price, evidence, route, [q.leg() for q in legs])


def current_price(asset: str, quotes: list[Quote], at: int):
    """§3 steps 3–4: a fresh direct price, else a fresh cross price."""
    direct = latest([q for q in quotes if q.base == asset and q.quote == "KRW" and public_quote(q, at)
                     and fresh(q.evidence, at)], by_source)
    if direct is not None and direct is not CONFLICT:
        return Price(parse(direct.price), direct.evidence, "direct:" + direct.venue, [direct.leg()])
    derived = cross(asset, quotes, at)
    return derived if derived is not None and fresh(derived.evidence, at) else None


def reference_price(asset: str, references: list[Quote], quotes: list[Quote], at: int):
    """An aged public price, at most 24 hours old and never counted."""
    direct = latest([q for q in references if q.base == asset and q.quote == "KRW" and public_quote(q, at)
                     and reference_age(q.evidence, at) and not fresh(q.evidence, at)], by_source)
    if direct is not None and direct is not CONFLICT:
        return Price(parse(direct.price), direct.evidence, "reference:" + direct.venue, [direct.leg()])
    derived = cross(asset, references + quotes, at)
    if derived is None or fresh(derived.evidence, at) or not reference_age(derived.evidence, at):
        return None
    derived.route = "reference:" + derived.route
    return derived


def convert(amount: Decimal, currency: str, quotes, references, at):
    """Returns (value, price, reference_value, reference_price)."""
    if not currency:
        return None, None, None, None
    if currency == "KRW":
        return amount, None, None, None
    if amount == 0:
        return Decimal(0), None, None, None
    price = current_price(currency, quotes, at)
    if isinstance(price, Price):
        worth = mul(amount, price.price)
        if fits(worth):
            return worth, price, None, None
    if price is CONFLICT:
        return None, None, None, None
    aged = reference_price(currency, references, quotes, at)
    if aged is None:
        return None, None, None, None
    worth = mul(amount, aged.price)
    return (None, None, worth, aged) if fits(worth) else (None, None, None, None)


# ---------------------------------------------------------------- valuation (§2, §4, §5)

@dataclass
class Row:
    account_scope: str
    connection_id: str
    market: str
    component_id: str
    asset: str
    kind: str
    quantity: str
    state: str = "Unvalued"  # Fresh | Reference | NotCounted | Unvalued
    value: Decimal | None = None
    reference_value: Decimal | None = None
    reason: str = ""
    presence: str = "NeedsCheck"
    price: Price | None = None
    provider_pnl: str = ""
    provider_pnl_currency: str = ""
    excluded_by_user: bool = False


def presence(c: Component, s: Snapshot) -> str:  # A9
    values = [parse(c.quantity)] + ([parse(c.reported_value)] if c.reported_value else [])
    if any(v is not None and v != 0 for v in values):
        return "Held"
    if any(v is None for v in values) or not c.evidence.complete:
        return "NeedsCheck"
    if c.kind == "Equity":
        pnl = parse(c.provider_unrealized_pnl) if c.provider_unrealized_pnl else None
        if not (s.balance_complete and pnl is not None and pnl == 0):
            return "NeedsCheck"
    return "Zero"


def provider_pnl(c: Component, s: Snapshot, at: int) -> tuple[str, str]:  # §11, A8
    e = c.evidence
    if (parse(c.provider_unrealized_pnl) is not None and c.provider_pnl_currency and e.complete and bool(e.source)
            and e.scope == s.account_scope and within(s.time_ms, at) and within(e.received_at_ms, at)
            and (e.source_time_ms == 0 or within(e.source_time_ms, at))):
        return c.provider_unrealized_pnl, c.provider_pnl_currency
    return "", ""


@dataclass
class Valuation:
    at: int
    complete: bool
    subtotal: Decimal | None
    rows: list[Row] = field(default_factory=list)
    issues: list[str] = field(default_factory=list)

    @property
    def net_assets(self) -> Decimal | None:
        return self.subtotal if self.complete else None


def canonical_mint(asset: str) -> bool:
    if not isinstance(asset, str) or not asset.startswith("solana:"):
        return False
    mint, alphabet = asset[7:], "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    if not 32 <= len(mint) <= 44 or any(ch not in alphabet for ch in mint):
        return False
    number = 0
    for ch in mint:
        number = number * 58 + alphabet.index(ch)
    zeroes = len(mint) - len(mint.lstrip("1"))
    return zeroes + (number.bit_length() + 7) // 8 == 32


def value(snapshots: list[Snapshot], quotes: list[Quote], references: list[Quote], at: int,
          excluded_assets=()) -> Valuation:
    result = Valuation(at, bool(snapshots), Decimal(0))
    seen = set()

    def fail(issue: str):
        result.complete = False
        if issue not in result.issues:
            result.issues.append(issue)

    for s in snapshots:
        if not s.account_scope or s.account_scope in seen:  # A12
            fail("계좌 범위 중복 또는 누락")
            continue
        seen.add(s.account_scope)
        balance_ok = (s.balance_complete and fresh(s.evidence, at) and within(s.time_ms, at))
        if not balance_ok:
            fail("잔고 조회 미완료 또는 180초 초과")
        equity_groups = {c.valuation_group for c in s.components if c.kind == "Equity"}
        ids, equity_assets = set(), set()
        for c in s.components:
            row = Row(s.account_scope, s.connection_id, s.market, c.id, c.asset, c.kind, c.quantity,
                      presence=presence(c, s))
            row.provider_pnl, row.provider_pnl_currency = provider_pnl(c, s, at)
            equity_key = (c.valuation_group, c.asset)
            duplicate_equity = c.kind == "Equity" and equity_key in equity_assets
            if c.kind == "Equity":
                equity_assets.add(equity_key)
            result.rows.append(row)
            if not c.id or c.id in ids:
                fail("행 ID 중복 또는 누락")
                row.reason = "중복 자산 행"
                continue
            ids.add(c.id)
            covered = c.kind in BASE_KINDS and c.valuation_group in equity_groups
            if not (c.kind == "Equity" or (c.kind in BASE_KINDS and not covered)):
                row.state, row.reason = "NotCounted", "합계 대상 아님"
                continue
            if not c.valuation_group:
                fail("평가 그룹 없음")
                row.reason = "평가 그룹 없음"
                continue
            if duplicate_equity:
                fail("동일 범위·통화 equity 중복")
                row.reason = "동일 범위·통화 equity 중복"
                continue
            if canonical_mint(c.asset) and c.asset in excluded_assets:
                row.excluded_by_user, row.state, row.reason = True, "NotCounted", "사용자 평가 대상 제외"
                continue
            amount = parse(c.reported_value) if c.reported_value else parse(c.quantity)
            # §2: account completeness only decides the verdict; each row stands on its own evidence.
            if not fresh(c.evidence, at) or amount is None:
                fail("수량·근거 미확인")
                row.reason = "수량·근거 미확인 또는 180초 초과"
                continue
            if c.kind == "Liability":
                amount = amount.copy_abs().copy_negate()
            currency = c.reported_value_currency if c.reported_value else c.asset
            worth, price, aged, aged_price = convert(amount, currency, quotes, references, at)
            if worth is None:
                fail("평가 가격 없음")
                row.reason = "평가 가격 없음"
                if aged is not None:
                    row.state, row.reference_value, row.price = "Reference", aged, aged_price
                    row.reason = "참고 평가 · 합계 제외"
                continue
            if not fits(worth):
                fail("금액 정밀도 상한 초과")
                row.reason = "금액 정밀도 상한 초과"
                continue
            result.subtotal = add(result.subtotal, worth)
            row.state, row.value, row.price = "Fresh", worth, price
    if not fits(result.subtotal):
        fail("금액 정밀도 상한 초과")
        result.subtotal = None
    for row in result.rows:
        if row.kind == "Equity" and row.presence == "Zero" and row.state != "Fresh":
            row.presence = "NeedsCheck"
    return result


# ---------------------------------------------------------------- presentation (§4, §6–§10)

def identity(market: str, asset: str) -> str:  # §6
    if asset == "KRW":
        return "cash:KRW"
    if market == "domestic_equity":
        return "stock:KR:" + asset
    if market == "solana":
        return "solana:native" if asset == "SOL" else asset
    return "exchange:" + asset


@dataclass
class Group:
    identity: str
    asset: str
    rows: list[Row] = field(default_factory=list)

    @property
    def has_fresh(self) -> bool:
        return any(r.state == "Fresh" for r in self.rows)

    @property
    def fresh_total(self) -> Decimal | None:
        return bounded_sum(r.value for r in self.rows if r.state == "Fresh")

    @property
    def zero(self) -> bool:
        return all(r.presence == "Zero" for r in self.rows)

    @property
    def quantity(self) -> Decimal | None:
        quantities = []
        for r in self.rows:
            if r.state == "NotCounted" and not r.excluded_by_user:
                continue
            q = parse(r.quantity)
            if q is None:
                return None
            quantities.append(q.copy_abs().copy_negate() if r.kind == "Liability" else q)
        return bounded_sum(quantities) if quantities else None

    @property
    def allocation(self) -> Decimal | None:  # §7 numerator
        total = Decimal(0)
        for r in self.rows:
            if r.state == "Fresh" and r.kind != "Liability" and r.value > 0:
                total = bounded_add(total, r.value)
        return total


def groups_of(rows: list[Row]) -> list[Group]:
    groups: dict[str, Group] = {}
    for r in rows:
        key = identity(r.market, r.asset)
        groups.setdefault(key, Group(key, r.asset)).rows.append(r)

    def rank(g: Group):  # A10
        tier = 2 if g.zero else 0 if g.has_fresh else 1
        return (tier, g.fresh_total.copy_negate() if tier == 0 and g.fresh_total is not None else Decimal(0), g.identity)
    return sorted(groups.values(), key=rank)


def share(part: Decimal | None, total: Decimal | None) -> str:  # §7
    if part is None or total is None or total <= 0 or part < 0 or part > total:
        return "—"
    # Exact half-up of part/total in tenths of a percent: floor((2000·part + total) / 2·total).
    tenths = int(EXACT.divide_int(add(mul(part, Decimal(2000)), total), mul(total, Decimal(2))))
    if tenths == 0 and part > 0:
        return "<0.1%"
    return f"{tenths // 10}.{tenths % 10}%"


def usdt_threshold(quotes: list[Quote], at: int) -> Decimal | None:  # §9
    pick = latest([q for q in quotes if q.venue == "upbit" and q.base == "USDT" and q.quote == "KRW"
                   and q.evidence.scope == "public:upbit:KRW-USDT" and q.evidence.source_time_ms > 0
                   and public_quote(q, at) and fresh(q.evidence, at)], by_source)
    return None if pick is None or pick is CONFLICT else parse(pick.price)


def dust(groups: list[Group], threshold: Decimal | None) -> tuple[list[str], Decimal]:
    hidden, total = [], Decimal(0)
    if threshold is None:
        return hidden, total
    for g in groups:
        safe = g.rows and all(r.state == "Fresh" and r.presence != "NeedsCheck" and r.value is not None
                              and not r.quantity.startswith("-") for r in g.rows)
        if safe and g.fresh_total is not None and g.fresh_total < threshold:
            hidden.append(g.identity)
            total = add(total, g.fresh_total)
    return hidden, total


def evaluate(bundle: Bundle, at: int | None = None) -> dict:
    at = bundle.as_of_ms if at is None else at
    connections = [c for c in bundle.connections if c.get("environment") in LIVE]  # §0
    snapshots = [s for s in bundle.snapshots if s.environment in LIVE]
    v = value(snapshots, bundle.quotes, bundle.references, at, bundle.excluded_assets)
    screen_complete = (v.complete and bool(connections)  # §4
                       and all(c.get("state") == "complete" for c in connections))
    groups = groups_of(v.rows)
    excluded_groups = sum(any(r.excluded_by_user for r in g.rows) for g in groups)
    any_fresh = any(r.state == "Fresh" for r in v.rows)
    partial_label = "확인된 자산 부분합" if bundle.screen_presentation == 2 else "평가액"
    if bundle.loading:
        label, figure = "로딩", "—"
    elif screen_complete:
        label, figure = "순자산", krw(v.subtotal)
    elif any_fresh and v.subtotal is None:
        label, figure = partial_label, "자료 부족"
    elif any_fresh and v.subtotal is not None:
        label, figure = partial_label, krw(v.subtotal)
    else:
        label, figure = partial_label, "—" if bundle.screen_presentation == 2 else "미평가"
    if excluded_groups and not bundle.loading:
        label = "평가 대상 합계" if screen_complete else "평가 대상 부분합"
    allocation_total = Decimal(0)
    for g in groups:
        allocation_total = bounded_add(allocation_total, g.allocation)
    slices = sorted([(g, g.allocation) for g in groups if g.allocation is not None and g.allocation > 0], key=lambda p: -p[1])
    bar = [{"identity": g.identity, "value": text(a), "share": share(a, allocation_total)} for g, a in slices]
    if len(bar) >= 6:
        rest = Decimal(0)
        for _, a in slices[4:]:
            rest = add(rest, a)
        bar = bar[:4] + [{"identity": "기타", "value": text(rest), "share": share(rest, allocation_total)}]
    threshold = usdt_threshold(bundle.quotes, at)
    hidden, hidden_total = dust(groups, threshold)
    reference_total = bounded_sum(r.reference_value for r in v.rows if r.state == "Reference")
    return {
        "format": "keefi-valuation-result/1",
        "as_of_ms": at,
        "policy": "docs/keefi/VALUATION-POLICY.md (G7 v2, 2026-10-21)",
        "valuation": {"complete": v.complete, "net_assets": None if v.net_assets is None else text(v.net_assets),
                      "known_subtotal": None if v.subtotal is None else text(v.subtotal), "issues": sorted(v.issues)},
        "screen": {"complete": screen_complete, "label": label, "figure": figure,
                   "excluded_groups": excluded_groups,
                   "displayed_saved_view": False,
                   "reference_total": None if reference_total is None else text(reference_total),
                   "unvalued_groups": sum(any(r.state == "Unvalued" for r in g.rows) for g in groups),
                   "reference_groups": sum(any(r.state == "Reference" for r in g.rows) for g in groups)},
        "groups": [{
            "identity": g.identity, "asset": g.asset,
            "quantity": None if g.quantity is None else text(g.quantity),
            "fresh_total": text(g.fresh_total) if g.has_fresh and g.fresh_total is not None else None,
            "fresh_total_krw": krw(g.fresh_total) if g.has_fresh and g.fresh_total is not None else None,
            "partial": g.has_fresh and any(r.state in ("Unvalued", "Reference") for r in g.rows),
            "share": share(g.allocation, allocation_total) if g.allocation is not None and g.allocation > 0 else "—",
            "rows": [{
                "account_scope": r.account_scope, "component_id": r.component_id, "kind": r.kind,
                "quantity": r.quantity, "state": r.state, "presence": r.presence,
                "value": None if r.value is None else text(r.value),
                "value_krw": None if r.value is None else krw(r.value),
                "reference_value": None if r.reference_value is None else text(r.reference_value),
                "excluded_by_user": r.excluded_by_user,
                "reason": r.reason,
                "price": None if r.price is None else {"route": r.price.route, "price": text(r.price.price),
                                                       "evidence": r.price.evidence.as_dict(), "legs": r.price.legs},
                "provider_pnl": r.provider_pnl or None, "provider_pnl_currency": r.provider_pnl_currency or None,
            } for r in g.rows],
        } for g in groups],
        "allocation": {"total": None if allocation_total is None else text(allocation_total), "bar": bar},
        "dust": {"threshold": None if threshold is None else text(threshold),
                 "hidden": hidden, "hidden_total": text(hidden_total),
                 "subtotal_after_hiding": None if v.subtotal is None else text(add(v.subtotal, hidden_total.copy_negate()))},
        "hide_unvalued": [g.identity for g in groups if g.rows and all(r.state == "Unvalued" for r in g.rows)],
        "assumptions": ASSUMPTIONS,
        "policy_decisions": POLICY_DECISIONS,
    }


def recorded(bundle: Bundle, now: int) -> dict | None:
    """§12: a saved view valued at the latest saved time; any time after now discards it."""
    connections = [dict(c) for c in bundle.connections]
    connection_ids = {c.get('id') for c in connections}
    snapshots = [s for s in bundle.snapshots if s.connection_id in connection_ids]
    scopes = {s.account_scope for s in snapshots}
    quotes = [q for q in bundle.quotes if q.evidence.scope in scopes or q.evidence.scope.startswith('public:')]
    times = []
    for s in snapshots:
        times += [s.time_ms, s.evidence.source_time_ms, s.evidence.received_at_ms]
        for c in s.components:
            times += [c.evidence.source_time_ms, c.evidence.received_at_ms]
    for q in quotes:
        times += [q.evidence.source_time_ms, q.evidence.received_at_ms]
    if not snapshots or any(t < 0 or t > now for t in times) or max(times) <= 0:
        return None
    saved_connections = {s.connection_id for s in snapshots}
    for connection in connections:
        connection['active'] = False
        connection['state'] = 'complete' if connection.get('id') in saved_connections else 'not_read'
    saved = Bundle(max(times), connections, snapshots, quotes, [], False, bundle.excluded_assets, bundle.screen_presentation)
    result = evaluate(saved)
    result['screen']['displayed_saved_view'] = True
    result['screen']['label'] = ('마지막 확인 평가 대상 합계' if result['screen']['excluded_groups']
                                 else '마지막 확인 자산' if result['screen']['complete'] else '마지막 확인 평가액')
    return result


def report(result: dict) -> str:
    lines = [f"{result['screen']['label']}  {result['screen']['figure']}"
             + ("" if result["screen"]["figure"] in ("—", "미평가", "자료 부족") else " KRW"),
             f"정확한 부분합 {result['valuation']['known_subtotal']} · 판정 {'완전' if result['valuation']['complete'] else '미완료'}"]
    if result["valuation"]["issues"]:
        lines.append("빠진 이유: " + " · ".join(result["valuation"]["issues"]))
    for g in result["groups"]:
        states = {}
        for r in g["rows"]:
            states[r["state"]] = states.get(r["state"], 0) + 1
        figure = g["fresh_total_krw"] or "—"
        lines.append(f"  {g['identity']:<28} 수량 {g['quantity'] or '—':>20}  평가 {figure:>16}  비중 {g['share']:>6}  "
                     + " ".join(f"{k}{n}" for k, n in sorted(states.items())) + (" · 일부 제외" if g["partial"] else ""))
    dust_info = result["dust"]
    if dust_info["threshold"]:
        lines.append(f"소액 기준 1 USDT = {dust_info['threshold']} KRW · 숨김 {len(dust_info['hidden'])}묶음 "
                     f"{dust_info['hidden_total']} KRW")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", help=f"{FORMAT} JSON file")
    parser.add_argument("--as-of", type=int, help="evaluation time in ms (default: bundle as_of_ms)")
    parser.add_argument("--recorded", action="store_true", help="evaluate as the saved view of §12")
    parser.add_argument("--json", action="store_true", help="print the full result as JSON")
    args = parser.parse_args(argv)
    with open(args.bundle, encoding="utf-8") as f:
        bundle = Bundle.load(json.load(f))
    if args.recorded:
        result = recorded(bundle, args.as_of or bundle.as_of_ms)
        if result is None:
            print("저장 화면 없음: 시각이 없거나 기준 시각보다 늦은 근거가 있습니다.")
            return 1
    else:
        result = evaluate(bundle, args.as_of)
    print(json.dumps(result, ensure_ascii=False, indent=2) if args.json else report(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
