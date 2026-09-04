"""Structured logging.

One JSON object per line on stdout, which is what a container wants. Stdlib
only -- PLAN.md section 12 says no dependencies beyond requirements.txt without
a recorded reason, and a log formatter is not a reason.

Attach context with the `extra` dict as usual:

    log.info("committed version", extra={"game_id": "GALE01", "version": 8})
"""

from __future__ import annotations

import datetime as _dt
import json
import logging
import sys

#: LogRecord attributes that are part of the machinery rather than the event.
#: Anything else a caller passes via `extra` is emitted as a top-level field.
_RESERVED = frozenset(
    """
    args asctime created exc_info exc_text filename funcName levelname levelno
    lineno module msecs message msg name pathname process processName
    relativeCreated stack_info taskName thread threadName
    """.split()
) | {
    # uvicorn passes an ANSI-coloured duplicate of the message through `extra`.
    # Useful for its own console handler, noise in a JSON line.
    "color_message",
}


class JsonFormatter(logging.Formatter):
    """Render a LogRecord as a single-line JSON object."""

    def format(self, record: logging.LogRecord) -> str:
        payload = {
            "ts": _dt.datetime.fromtimestamp(
                record.created, tz=_dt.UTC
            ).isoformat(timespec="milliseconds"),
            "level": record.levelname,
            "logger": record.name,
            "msg": record.getMessage(),
        }

        for key, value in record.__dict__.items():
            if key not in _RESERVED and not key.startswith("_"):
                payload[key] = _safe(value)

        if record.exc_info:
            payload["exc"] = self.formatException(record.exc_info)
        if record.stack_info:
            payload["stack"] = self.formatStack(record.stack_info)

        return json.dumps(payload, default=str, separators=(",", ":"))


def _safe(value):
    """Keep JSON-native types as they are; stringify everything else."""
    if isinstance(value, str | int | float | bool | None):
        return value
    if isinstance(value, list | tuple):
        return [_safe(v) for v in value]
    if isinstance(value, dict):
        return {str(k): _safe(v) for k, v in value.items()}
    return str(value)


def configure(level: str = "INFO") -> None:
    """Install the JSON formatter on the root logger.

    Idempotent: replaces any handlers we installed earlier so repeated calls in
    tests do not multiply output.
    """
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(JsonFormatter())

    root = logging.getLogger()
    root.handlers = [handler]
    root.setLevel(getattr(logging, level, logging.INFO))

    # uvicorn installs its own colourful handlers; make them delegate to ours so
    # request logs land in the same stream in the same shape.
    for name in ("uvicorn", "uvicorn.error", "uvicorn.access"):
        logger = logging.getLogger(name)
        logger.handlers = []
        logger.propagate = True
