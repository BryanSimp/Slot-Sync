"""Server-rendered web UI -- PLAN.md section 9.

Jinja2 templates and a little vanilla JS. No build step and no frontend
framework, by decision; the CSS lives in `base.html` so there is no static
mount either.

The one place JS is doing real work is the upload form, which has to hold the
selected file across a conflict so "keep mine" can re-send it. Everything else
is links and form posts and works with JS off.
"""

from __future__ import annotations

import datetime as dt
import logging
from pathlib import Path

from fastapi import APIRouter, Form, Request
from fastapi.responses import HTMLResponse, RedirectResponse, Response
from fastapi.templating import Jinja2Templates

from .api import COOKIE_NAME, token_is_valid
from .store import NotFoundError, Store, ValidationError

log = logging.getLogger("slotsync.web")

router = APIRouter()

TEMPLATES = Jinja2Templates(directory=str(Path(__file__).parent / "templates"))

# Route names are prefixed `web_` because `url_for` resolves names across the
# whole app and api.py already has a `card_detail`. Without the prefix the UI's
# redirects land on the JSON endpoint instead of the page.

#: A month. The token is the only credential and it does not rotate, so a short
#: session would just mean retyping it for no gain.
COOKIE_MAX_AGE = 60 * 60 * 24 * 30


# --- helpers --------------------------------------------------------------


def _authed(request: Request) -> bool:
    return token_is_valid(request, request.cookies.get(COOKIE_NAME, ""))


def _login_redirect(request: Request) -> RedirectResponse:
    return RedirectResponse(
        request.url_for("web_login_form").include_query_params(next=request.url.path),
        status_code=303,
    )


def _store(request: Request) -> Store:
    return request.app.state.store


def _timestamp(value: int | None) -> str:
    if not value:
        return "—"
    return dt.datetime.fromtimestamp(value, tz=dt.UTC).strftime("%Y-%m-%d %H:%M UTC")


def _filesize(value: int | None) -> str:
    if value is None:
        return "—"
    if value >= 1024 * 1024:
        return f"{value / (1024 * 1024):.0f} MiB"
    return f"{value / 1024:.0f} KiB"


TEMPLATES.env.filters["timestamp"] = _timestamp
TEMPLATES.env.filters["filesize"] = _filesize


# --- auth -----------------------------------------------------------------


@router.get("/login", response_class=HTMLResponse, name="web_login_form")
def login_form(request: Request, next: str = "/") -> Response:
    return TEMPLATES.TemplateResponse(
        request, "login.html", {"next": next, "error": None}
    )


@router.post("/login")
def login(
    request: Request, token: str = Form(...), next: str = Form(default="/")
) -> Response:
    if not token_is_valid(request, token):
        log.warning("web login rejected", extra={"client": request.client.host})
        return TEMPLATES.TemplateResponse(
            request,
            "login.html",
            {"next": next, "error": "That token does not match."},
            status_code=401,
        )

    # Only redirect within this app -- an absolute `next` would make this an
    # open redirect.
    target = next if next.startswith("/") and not next.startswith("//") else "/"
    response = RedirectResponse(target, status_code=303)
    response.set_cookie(
        COOKIE_NAME,
        token,
        max_age=COOKIE_MAX_AGE,
        httponly=True,
        samesite="lax",
    )
    return response


@router.post("/logout")
def logout() -> Response:
    response = RedirectResponse("/login", status_code=303)
    response.delete_cookie(COOKIE_NAME)
    return response


# --- pages ----------------------------------------------------------------


@router.get("/", response_class=HTMLResponse, name="web_card_list")
def card_list(request: Request) -> Response:
    if not _authed(request):
        return _login_redirect(request)

    return TEMPLATES.TemplateResponse(
        request, "cards.html", {"cards": _store(request).list_cards()}
    )


@router.get(
    "/cards/{game_id}/{slot}", response_class=HTMLResponse, name="web_card_detail"
)
def card_detail(request: Request, game_id: str, slot: str) -> Response:
    if not _authed(request):
        return _login_redirect(request)

    store = _store(request)
    head = store.head(game_id, slot)
    if head is None:
        raise NotFoundError(f"no card stored for {game_id} slot {slot}")

    # A stored card can still be one the parser dislikes -- it was accepted with
    # warnings. Show what we can and say so rather than failing the page.
    try:
        card = store.inspect_version(head)
        parse_error = None
    except ValidationError as exc:
        card, parse_error = None, str(exc)

    return TEMPLATES.TemplateResponse(
        request,
        "card.html",
        {
            "head": head,
            "card": card,
            "parse_error": parse_error,
            "history": store.history(game_id, slot),
        },
    )


@router.post("/cards/{game_id}/{slot}/rollback/{version}", name="web_rollback")
def rollback(request: Request, game_id: str, slot: str, version: int) -> Response:
    """Rollback as a plain form POST, so it works with JS disabled."""
    if not _authed(request):
        return _login_redirect(request)

    _store(request).rollback(game_id, slot, version)
    return RedirectResponse(
        request.url_for("web_card_detail", game_id=game_id, slot=slot), status_code=303
    )
