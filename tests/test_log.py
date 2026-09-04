"""M0: logs are one JSON object per line."""

from __future__ import annotations

import json
import logging

from slotsync.log import JsonFormatter


def _record(**extra) -> logging.LogRecord:
    record = logging.LogRecord(
        name="slotsync.test",
        level=logging.INFO,
        pathname=__file__,
        lineno=1,
        msg="pushed %s",
        args=("GALE01",),
        exc_info=None,
    )
    record.__dict__.update(extra)
    return record


def test_format_is_a_single_json_line():
    line = JsonFormatter().format(_record())
    assert "\n" not in line

    payload = json.loads(line)
    assert payload["level"] == "INFO"
    assert payload["logger"] == "slotsync.test"
    assert payload["msg"] == "pushed GALE01"
    assert payload["ts"].endswith("+00:00")


def test_extra_fields_become_top_level_keys():
    payload = json.loads(JsonFormatter().format(_record(game_id="GALE01", version=8)))
    assert payload["game_id"] == "GALE01"
    assert payload["version"] == 8


def test_unserialisable_values_do_not_break_the_line():
    payload = json.loads(JsonFormatter().format(_record(path=object())))
    assert isinstance(payload["path"], str)


def test_exception_info_is_included():
    try:
        raise ValueError("boom")
    except ValueError:
        import sys

        record = _record()
        record.exc_info = sys.exc_info()
        payload = json.loads(JsonFormatter().format(record))

    assert "ValueError: boom" in payload["exc"]


def test_uvicorn_colour_duplicate_is_dropped():
    """uvicorn ships an ANSI copy of the message in `extra`; it is noise here."""
    payload = json.loads(JsonFormatter().format(_record(color_message="\x1b[36m%d")))
    assert "color_message" not in payload
