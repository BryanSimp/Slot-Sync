"""HTTP API -- PLAN.md section 8.

JSON for metadata, `application/octet-stream` for card images. Bearer token in
`Authorization`, taken from the environment.

This is the Dolphin daemon's ingest path and the one you reach for with curl.
The console path is the binary UDP protocol; both land in `store.Store`.
"""

from __future__ import annotations

import hmac
import logging

from fastapi import APIRouter, Depends, Header, Query, Request, Response

from .memcard import DEFAULT_MBIT, Card, MemcardError, Save, format_card
from .store import (
    CardSummary,
    ConflictError,
    Device,
    NotFoundError,
    Store,
    TooLargeError,
    ValidationError,
    Version,
    normalise_game_id,
    normalise_slot,
)

log = logging.getLogger("slotsync.api")

router = APIRouter(prefix="/api")


# --- auth -----------------------------------------------------------------


#: Where the browser keeps the same shared token. Still one secret and no
#: accounts -- a cookie is just the only credential a plain <a href> or form
#: POST can carry. See PLAN.md section 9.
COOKIE_NAME = "slotsync_token"


def token_is_valid(request: Request, presented: str) -> bool:
    """Check the token, rate-limiting wrong answers.

    compare_digest, so the token cannot be recovered by timing.

    The comparison happens *before* the budget is consulted, and only a wrong
    answer costs anything. Checking the limiter first would lock out a correct
    token too, which punishes the household member who mistyped it in the web
    UI while doing nothing to an attacker -- who does not have the token and is
    throttled either way. A cheap `compare_digest` is not worth protecting from
    the way an expensive hash would be.
    """
    limiter = getattr(request.app.state, "auth_limiter", None)
    client = request.client.host if request.client else "unknown"

    if hmac.compare_digest(presented, request.app.state.config.token):
        if limiter is not None:
            limiter.reset(client)
        return True

    if limiter is not None and not limiter.allow(client):
        raise AuthThrottled()
    return False


def presented_token(request: Request, authorization: str | None) -> str:
    """The token from the Authorization header, falling back to the cookie."""
    scheme, _, value = (authorization or "").partition(" ")
    if scheme.lower() == "bearer" and value:
        return value
    return request.cookies.get(COOKIE_NAME, "")


def require_token(
    request: Request, authorization: str | None = Header(default=None)
) -> None:
    """Check the shared token.

    Single household, one token, no accounts -- PLAN.md section 11. Machine
    clients send `Authorization: Bearer`; the web UI sends the cookie the login
    form set. The cookie is SameSite=Lax, so a cross-site form POST cannot ride
    on it.
    """
    if not token_is_valid(request, presented_token(request, authorization)):
        raise AuthError()


class AuthError(Exception):
    """Missing or wrong bearer token. Translated to 401 in app.py."""


class AuthThrottled(Exception):
    """Too many failed authentications from one client. 429 in app.py."""


def get_store(request: Request) -> Store:
    return request.app.state.store


# --- serialisation --------------------------------------------------------


def version_json(version: Version) -> dict:
    return {
        "game_id": version.game_id,
        "slot": version.slot,
        "slot_name": version.slot_name,
        "version": version.version,
        "parent": version.parent,
        "sha256": version.sha256,
        "size": version.size,
        "device_id": version.device_id,
        "created_at": version.created_at,
        "note": version.note,
    }


def card_json(card: CardSummary) -> dict:
    return {
        "game_id": card.game_id,
        "slot": card.slot,
        "slot_name": card.slot_name,
        "head": card.head,
        "sha256": card.sha256,
        "size": card.size,
        "device_id": card.device_id,
        "updated_at": card.updated_at,
        "versions": card.versions,
    }


def card_format_json(card: Card) -> dict:
    """The card as a format, rather than as a stored version."""
    return {
        "size_bytes": card.size_bytes,
        "size_mbits": card.size_mbits,
        "data_blocks": card.data_blocks,
        "free_blocks": card.free_blocks,
        "used_blocks": card.used_blocks,
        "encoding": card.encoding_name,
        "formatted_at": (
            None if card.formatted_at is None else card.formatted_at.isoformat()
        ),
        "active_directory": card.active_directory,
        "active_bat": card.active_bat,
        # Present but non-fatal damage. A card with warnings is stored and the
        # UI says so; see PLAN.md section 5.
        "warnings": card.warnings,
    }


def save_json(save: Save) -> dict:
    """A directory entry: the real save name, not just a filename."""
    return {
        "index": save.index,
        "game_code": save.game_code,
        "maker_code": save.maker_code,
        "filename": save.filename,
        "title": save.title,
        "subtitle": save.subtitle,
        "display_name": save.display_name,
        "blocks": save.block_count,
        "size_bytes": save.size_bytes,
        "first_block": save.first_block,
        "modified": None if save.modified is None else save.modified.isoformat(),
        "no_copy": save.no_copy,
        "no_move": save.no_move,
        "copy_counter": save.copy_counter,
    }


def device_json(device: Device) -> dict:
    return {
        "device_id": device.device_id,
        "label": device.label,
        "kind": device.kind,
        "first_seen": device.first_seen,
        "last_seen": device.last_seen,
    }


def _raw_response(store: Store, version: Version) -> Response:
    """Serve a card image.

    The filename matches what Nintendont writes to the SD card, so a download
    can be dropped straight into /saves/.
    """
    data = store.read_image(version)
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={
            "Content-Disposition": f'attachment; filename="{version.game_id}.raw"',
            "ETag": f'"{version.sha256}"',
            "X-SlotSync-Version": str(version.version),
            "X-SlotSync-SHA256": version.sha256,
        },
    )


def _require_head(store: Store, game_id: str, slot: int | str) -> Version:
    head = store.head(game_id, slot)
    if head is None:
        gid, s = normalise_game_id(game_id), normalise_slot(slot)
        raise NotFoundError(f"no card stored for {gid} slot {'AB'[s]}")
    return head


# --- routes ---------------------------------------------------------------


@router.get("/cards", dependencies=[Depends(require_token)])
def list_cards(store: Store = Depends(get_store)) -> dict:
    return {"cards": [card_json(c) for c in store.list_cards()]}


@router.get("/cards/{game_id}/{slot}", dependencies=[Depends(require_token)])
def card_detail(game_id: str, slot: str, store: Store = Depends(get_store)) -> dict:
    head = _require_head(store, game_id, slot)
    history = store.history(game_id, slot)
    card = store.inspect_version(head)
    return {
        "game_id": head.game_id,
        "slot": head.slot,
        "slot_name": head.slot_name,
        "head": version_json(head),
        "versions": len(history),
        "card": card_format_json(card),
        "saves": [save_json(s) for s in card.saves],
    }


@router.get("/cards/{game_id}/{slot}/versions", dependencies=[Depends(require_token)])
def card_versions(game_id: str, slot: str, store: Store = Depends(get_store)) -> dict:
    history = store.history(game_id, slot)
    if not history:
        _require_head(store, game_id, slot)  # raises the 404 with a good message
    head = store.head(game_id, slot)
    return {
        "game_id": normalise_game_id(game_id),
        "slot": normalise_slot(slot),
        "head": None if head is None else head.version,
        "versions": [version_json(v) for v in history],
    }


# Declared before the {version}.raw route below: "latest" would otherwise be
# captured as a version identifier.
@router.get("/cards/{game_id}/{slot}/latest.raw", dependencies=[Depends(require_token)])
def card_latest_raw(
    game_id: str, slot: str, store: Store = Depends(get_store)
) -> Response:
    return _raw_response(store, _require_head(store, game_id, slot))


@router.get(
    "/cards/{game_id}/{slot}/{version}.raw", dependencies=[Depends(require_token)]
)
def card_version_raw(
    game_id: str, slot: str, version: str, store: Store = Depends(get_store)
) -> Response:
    return _raw_response(store, _parse_version(store, game_id, slot, version))


@router.post("/cards/{game_id}/{slot}", dependencies=[Depends(require_token)])
async def push_card(
    game_id: str,
    slot: str,
    request: Request,
    parent: int | None = Query(
        default=None,
        description="Version this image was derived from; 0 for a brand-new card.",
    ),
    device: int | None = Query(default=None, description="Optional u64 device id."),
    note: str | None = Query(default=None),
    store: Store = Depends(get_store),
) -> Response:
    """Push a whole card image.

    `parent` is required and deliberately has no default. Defaulting it to head
    would turn every stale push into a silent overwrite, which is the exact
    failure this project exists to prevent -- PLAN.md section 7.
    """
    if parent is None:
        raise ValidationError(
            "parent query parameter is required: pass the version you last "
            "pulled, or 0 for a new card"
        )

    _reject_oversized_declaration(request, store)
    body = await request.body()

    if device is not None:
        store.touch_device(device, kind="dolphin")

    result = store.push(game_id, slot, body, parent=parent, device_id=device, note=note)
    head = store.head(game_id, slot)

    return _json_response(
        {
            "game_id": head.game_id,
            "slot": head.slot,
            "version": result.version,
            "outcome": result.outcome,
            "sha256": head.sha256,
            "size": head.size,
        },
        status_code=201 if result.created else 200,
    )


@router.post(
    "/cards/{game_id}/{slot}/rollback/{version}", dependencies=[Depends(require_token)]
)
def rollback_card(
    game_id: str,
    slot: str,
    version: int,
    device: int | None = Query(default=None),
    store: Store = Depends(get_store),
) -> dict:
    """Make `version` head again, as a fresh version on top of history."""
    result = store.rollback(game_id, slot, version, device_id=device)
    head = store.head(game_id, slot)
    return {
        "game_id": head.game_id,
        "slot": head.slot,
        "version": result.version,
        "outcome": result.outcome,
        "restored_from": version,
        "sha256": head.sha256,
    }


@router.post("/cards/{game_id}/{slot}/format", dependencies=[Depends(require_token)])
def format_card_endpoint(
    game_id: str,
    slot: str,
    mbit: int = Query(default=DEFAULT_MBIT, description="Card size; 16 is the default."),
    device: int | None = Query(default=None),
    store: Store = Depends(get_store),
) -> Response:
    """Create a blank formatted card for a game the server has never seen.

    The PC side keeps one card per game (PLAN.md section 5), so a game with no
    history needs an empty card made before it can be played. Card-format
    knowledge stays here with the verified parser rather than being duplicated
    into every client.

    Refuses if a card already exists: overwriting one would be surprising even
    though history would survive it.
    """
    existing = store.head(game_id, slot)
    if existing is not None:
        raise ConflictError(existing.version, existing.sha256)

    try:
        image = format_card(mbit=mbit)
    except MemcardError as exc:
        raise ValidationError(str(exc)) from exc

    result = store.push(
        game_id, slot, image, parent=0, device_id=device, note=f"formatted {mbit} Mbit"
    )
    head = store.head(game_id, slot)
    return _json_response(
        {
            "game_id": head.game_id,
            "slot": head.slot,
            "version": result.version,
            "outcome": result.outcome,
            "sha256": head.sha256,
            "size": head.size,
            "mbit": mbit,
        },
        status_code=201,
    )


@router.get("/devices", dependencies=[Depends(require_token)])
def list_devices(store: Store = Depends(get_store)) -> dict:
    return {"devices": [device_json(d) for d in store.list_devices()]}


# --- helpers --------------------------------------------------------------


def _json_response(payload: dict, status_code: int = 200) -> Response:
    from fastapi.responses import JSONResponse

    return JSONResponse(payload, status_code=status_code)


def _parse_version(store: Store, game_id: str, slot: str, version: str) -> Version:
    if version == "latest":
        return _require_head(store, game_id, slot)
    try:
        number = int(version)
    except ValueError:
        raise ValidationError(
            f"version must be an integer or 'latest', got {version!r}"
        ) from None
    return store.get_version(game_id, slot, number)


def _reject_oversized_declaration(request: Request, store: Store) -> None:
    """Refuse an oversized body before reading it into memory.

    Content-Length is a claim, not a guarantee, so `Store.push` checks the real
    length too. This only saves us buffering an obviously bad request.
    """
    declared = request.headers.get("content-length")
    if declared is None:
        return
    try:
        length = int(declared)
    except ValueError:
        raise ValidationError(f"bad Content-Length: {declared!r}") from None
    if length > store.config.max_card_bytes:
        raise TooLargeError(
            f"card is {length} bytes, cap is {store.config.max_card_bytes}"
        )
