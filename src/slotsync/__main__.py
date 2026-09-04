"""Entry point. Runs the HTTP app and the UDP listener in one process.

M0: serve /healthz.
M4: bring up the UDP listener alongside.
"""

from __future__ import annotations

import logging
import sys

import uvicorn

from . import log as slog
from .app import create_app
from .config import Config, ConfigError


def main() -> None:
    try:
        config = Config.from_env()
    except ConfigError as exc:
        # Logging is not configured yet and this is an operator error, so say it
        # plainly on stderr rather than burying it in a JSON line.
        print(f"slotsync: configuration error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc

    slog.configure(config.log_level)
    log = logging.getLogger("slotsync")
    log.info(
        "starting",
        extra={"http_port": config.http_port, "udp_port": config.udp_port},
    )

    uvicorn.run(
        create_app(config),
        host=config.http_host,
        port=config.http_port,
        log_config=None,  # slog.configure() already owns the loggers
        access_log=True,
    )


if __name__ == "__main__":
    main()
