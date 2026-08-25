#!/usr/bin/env python3
"""Tests for the passive Codex (OpenAI) usage source.

codex_source reads rate-limit snapshots out of Codex CLI's local session logs
and turns them into the payload's optional "x" object. No network, no auth.

Run: python -m pytest daemon/tests/test_codex_source.py -x -q
"""
import json
import time
from pathlib import Path

import pytest

from daemon import codex_source


NOW = 1_787_500_000.0  # fixed "now" for deterministic reset math


def _token_count_line(used_percent=28.0, resets_at=None, window_minutes=10080,
                      secondary=None, plan="prolite",
                      input_tokens=1000, output_tokens=200):
    if resets_at is None:
        resets_at = NOW + 3600
    return json.dumps({
        "timestamp": "2026-08-18T21:12:00.427Z",
        "type": "event_msg",
        "payload": {
            "type": "token_count",
            "info": {
                "total_token_usage": {
                    "input_tokens": input_tokens,
                    "cached_input_tokens": 0,
                    "output_tokens": output_tokens,
                    "total_tokens": input_tokens + output_tokens,
                },
            },
            "rate_limits": {
                "limit_id": "codex",
                "primary": {
                    "used_percent": used_percent,
                    "window_minutes": window_minutes,
                    "resets_at": resets_at,
                },
                "secondary": secondary,
                "plan_type": plan,
            },
        },
    })


def _write_session(sessions: Path, when: float, name: str, lines: list[str]):
    t = time.localtime(when)
    day = sessions / f"{t.tm_year:04d}" / f"{t.tm_mon:02d}" / f"{t.tm_mday:02d}"
    day.mkdir(parents=True, exist_ok=True)
    path = day / name
    path.write_text("\n".join(lines) + "\n")
    return path


@pytest.fixture
def codex_home(tmp_path, monkeypatch):
    monkeypatch.setenv("CODEX_HOME", str(tmp_path))
    return tmp_path / "sessions"


def test_no_codex_dir_returns_none(codex_home):
    assert codex_source.codex_payload(now=NOW) is None


def test_no_rate_limits_event_returns_none(codex_home):
    _write_session(codex_home, NOW, "a.jsonl", ['{"type":"other_event"}'])
    assert codex_source.codex_payload(now=NOW) is None


def test_primary_only_plan(codex_home):
    _write_session(codex_home, NOW, "a.jsonl",
                   [_token_count_line(used_percent=28.0, resets_at=NOW + 90 * 60)])
    x = codex_source.codex_payload(now=NOW)
    assert x == {"p": 28.0, "rm": 90, "wm": 10080,
                 "ti": 1000, "to": 200, "da": 0}


def test_secondary_window_included(codex_home):
    secondary = {"used_percent": 61.5, "window_minutes": 10080,
                 "resets_at": NOW + 3 * 86400}
    _write_session(codex_home, NOW, "a.jsonl",
                   [_token_count_line(used_percent=12.0, resets_at=NOW + 600,
                                      window_minutes=300, secondary=secondary,
                                      plan="pro")])
    x = codex_source.codex_payload(now=NOW)
    assert x["p"] == 12.0 and x["rm"] == 10 and x["wm"] == 300
    assert x["p2"] == 61.5 and x["rm2"] == 3 * 1440 and x["wm2"] == 10080


def test_last_event_in_file_wins(codex_home):
    _write_session(codex_home, NOW, "a.jsonl",
                   [_token_count_line(used_percent=10.0),
                    _token_count_line(used_percent=55.0)])
    assert codex_source.codex_payload(now=NOW)["p"] == 55.0


def test_newest_file_wins(codex_home):
    older = _write_session(codex_home, NOW, "a.jsonl",
                           [_token_count_line(used_percent=10.0)])
    newer = _write_session(codex_home, NOW, "b.jsonl",
                           [_token_count_line(used_percent=70.0)])
    past = time.time() - 1000
    import os
    os.utime(older, (past, past))
    assert codex_source.codex_payload(now=NOW)["p"] == 70.0


def test_lapsed_window_reports_zero(codex_home):
    """A reset epoch in the past means the window rolled over since the last
    local Codex use: report 0% and an unknown countdown, never stale numbers."""
    _write_session(codex_home, NOW, "a.jsonl",
                   [_token_count_line(used_percent=93.0, resets_at=NOW - 60)])
    x = codex_source.codex_payload(now=NOW)
    assert x["p"] == 0.0 and x["rm"] == -1


def test_todays_tokens_sum_across_sessions_but_not_yesterdays(codex_home):
    _write_session(codex_home, NOW - 86400, "old.jsonl",
                   [_token_count_line(input_tokens=500, output_tokens=50)])
    _write_session(codex_home, NOW, "a.jsonl",
                   [_token_count_line(input_tokens=1000, output_tokens=200)])
    _write_session(codex_home, NOW, "b.jsonl",
                   [_token_count_line(input_tokens=30, output_tokens=7)])
    x = codex_source.codex_payload(now=NOW)
    assert x["ti"] == 1030 and x["to"] == 207


def test_yesterdays_rate_limits_found_after_midnight(codex_home):
    """Just after midnight, today's dir is empty; last night's snapshot must
    still surface (with today's token counters at zero)."""
    _write_session(codex_home, NOW - 86400, "evening.jsonl",
                   [_token_count_line(used_percent=42.0, resets_at=NOW + 7200)])
    x = codex_source.codex_payload(now=NOW)
    assert x["p"] == 42.0 and x["rm"] == 120
    assert x["ti"] == 0 and x["to"] == 0


def test_daily_average_over_active_prior_days(codex_home):
    """"da" averages total tokens over the prior 7 days' active days only;
    today's own usage is excluded from the baseline."""
    _write_session(codex_home, NOW, "today.jsonl",
                   [_token_count_line(input_tokens=999, output_tokens=1)])
    _write_session(codex_home, NOW - 1 * 86400, "d1.jsonl",
                   [_token_count_line(input_tokens=1000, output_tokens=200)])
    _write_session(codex_home, NOW - 3 * 86400, "d3.jsonl",
                   [_token_count_line(input_tokens=500, output_tokens=100)])
    # days 2, 4-7 have no sessions and must not drag the average down
    x = codex_source.codex_payload(now=NOW)
    assert x["da"] == (1200 + 600) // 2


def test_daily_average_zero_without_history(codex_home):
    _write_session(codex_home, NOW, "today.jsonl", [_token_count_line()])
    assert codex_source.codex_payload(now=NOW)["da"] == 0


def test_corrupt_lines_are_skipped(codex_home):
    _write_session(codex_home, NOW, "a.jsonl",
                   ['not json at all {{{',
                    _token_count_line(used_percent=33.0)])
    assert codex_source.codex_payload(now=NOW)["p"] == 33.0
