"""FastAPI application factory.

Later milestones mount more onto the app built here; the factory shape keeps
tests from needing a live server.
"""

from __future__ import annotations

import logging

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from . import __version__
from .api import AuthError
from .api import router as api_router
from .config import Config
from .store import (
    ConflictError,
    NotFoundError,
    Store,
    StoreError,
    TooLargeError,
    ValidationError,
)
from .web import router as web_router

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
    app.state.store = Store(config)

    @app.get("/healthz")
    def healthz() -> dict:
        """Liveness probe. No auth, by design -- see PLAN.md section 8."""
        return {"status": "ok", "version": __version__}

    app.include_router(api_router)
    # The UI is mounted last: its routes are the catch-all ones.
    app.include_router(web_router)
    _install_error_handlers(app)

    log.info(
        "application ready",
        extra={"data_dir": str(config.data_dir), "version": __version__},
    )
    return app


def _install_error_handlers(app: FastAPI) -> None:
    """Translate store errors into the status codes PLAN.md section 8 promises."""

    @app.exception_handler(AuthError)
    async def _auth(request: Request, exc: AuthError) -> JSONResponse:
        return JSONResponse(
            {"error": "unauthorized", "detail": "missing or invalid bearer token"},
            status_code=401,
            headers={"WWW-Authenticate": "Bearer"},
        )

    @app.exception_handler(ConflictError)
    async def _conflict(request: Request, exc: ConflictError) -> JSONResponse:
        # A 409 must carry the current head and its digest so the client can
        # show a real choice instead of guessing -- PLAN.md sections 7 and 8.
        return JSONResponse(
            {
                "error": "conflict",
                "detail": str(exc),
                "head": exc.head_version,
                "head_sha256": exc.head_sha256,
            },
            status_code=409,
        )

    @app.exception_handler(ValidationError)
    async def _bad_request(request: Request, exc: ValidationError) -> JSONResponse:
        return JSONResponse({"error": "bad_request", "detail": str(exc)}, status_code=400)

    @app.exception_handler(NotFoundError)
    async def _not_found(request: Request, exc: NotFoundError) -> JSONResponse:
        return JSONResponse({"error": "not_found", "detail": str(exc)}, status_code=404)

    @app.exception_handler(TooLargeError)
    async def _too_large(request: Request, exc: TooLargeError) -> JSONResponse:
        return JSONResponse({"error": "too_large", "detail": str(exc)}, status_code=413)

    @app.exception_handler(StoreError)
    async def _store(request: Request, exc: StoreError) -> JSONResponse:
        log.exception("unhandled store error")
        return JSONResponse({"error": "internal", "detail": str(exc)}, status_code=500)
