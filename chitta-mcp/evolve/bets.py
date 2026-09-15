"""Preregister signed predictions and resolve them without moving the goalposts."""

from __future__ import annotations

from datetime import datetime, timezone

from .proposals import Proposal, number
from .store import MemoryStore, body


def register(
    proposal: Proposal,
    band_from_noise: float | None,
    store: MemoryStore,
    cycle_id: str = "",
    metric_bands: dict | None = None,
) -> tuple[str, dict]:
    if band_from_noise is not None and number(band_from_noise) < 0:
        raise ValueError("noise band must be nonnegative")
    delta = proposal.expected_gain["delta"]
    bet = dict(
        proposal_id=proposal.id,
        metric=proposal.expected_gain["metric"],
        predicted_delta=delta,
        direction="increase" if delta >= 0 else "decrease",
        band_from_noise=band_from_noise,
        registered_ts=datetime.now(timezone.utc).isoformat(),
        cycle_id=cycle_id,
        metric_bands=metric_bands or {},
    )
    return store.remember("forward-bet", bet), bet


def outcome(
    bet: dict, measured_delta: float | dict, evidence: str = "", existing_wisdom: str = ""
) -> dict:
    metric = bet["metric"]
    measured = measured_delta if isinstance(measured_delta, dict) else {metric: measured_delta}
    measured = {key: number(value) for key, value in measured.items()}
    unexpected = {
        key: value
        for key, value in measured.items()
        if key != metric
        and key in bet.get("metric_bands", {})
        and abs(value) > number(bet["metric_bands"][key])
    }
    if unexpected:
        return dict(
            outcome="surprise",
            measured_delta=measured,
            unexpected=unexpected,
            novel=False,  # Metric surprise alone does not establish useful new knowledge.
        )
    band = bet.get("band_from_noise")
    if metric not in measured or band is None:
        raise ValueError("a measured target delta and preregistered noise band are required")
    delta = measured[metric]
    signed = delta if bet["direction"] == "increase" else -delta
    confirmed = signed > number(band) and abs(delta - bet["predicted_delta"]) <= number(band)
    return dict(
        outcome="confirmed" if confirmed else "refuted", measured_delta=measured, novel=False
    )


def resolve(
    bet_id: str,
    measured_delta: float | dict,
    store: MemoryStore | None = None,
    evidence: str = "",
    existing_wisdom: str = "",
    bet: dict | None = None,
) -> dict:
    store = store or MemoryStore()
    if bet is None:
        matches = [
            body(r) for r in store.recall("forward-bet", query=bet_id) if str(r.get("id")) == bet_id
        ]
        if not matches:
            raise ValueError("forward bet not found: " + bet_id)
        bet = matches[0]
    result = outcome(bet, measured_delta, evidence, existing_wisdom)
    result.update(
        bet_id=bet_id,
        proposal_id=bet["proposal_id"],
        cycle_id=bet.get("cycle_id"),
        resolved_ts=datetime.now(timezone.utc).isoformat(),
    )
    result["memory_id"] = store.remember("bet-resolution", result)
    return result
