"""FastAPI application factory.

M0 serves only `/healthz`. Later milestones mount the API and the web UI onto
the app built here; the factory shape keeps tests from needing a live server.
"""

from __future__ import annotations

import logging

from fastapi import FastAPI

from . import __version__
from .config import Config

log = logging.getLogger("slotsync.app")


def create_app(config: Config) -> FastAPI:
    """Build the application for `config`."""
    app = FastAPI(
        title="SlotSync",
        version=__version__,
        docs_url=None,
        redoc_url=None,
        openapi_url=None,
    )
    app.state.config = config

    config.data_dir.mkdir(parents=True, exist_ok=True)
    config.blobs_dir.mkdir(parents=True, exist_ok=True)

    @app.get("/healthz")
    def healthz() -> dict:
        """Liveness probe. No auth, by design -- see PLAN.md section 8."""
        return {"status": "ok", "version": __version__}

    log.info(
        "application ready",
        extra={"data_dir": str(config.data_dir), "version": __version__},
    )
    return app
