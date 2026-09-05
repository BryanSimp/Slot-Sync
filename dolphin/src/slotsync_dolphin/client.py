"""HTTP client for the SlotSync server.

Stdlib only, on purpose. This runs on somebody's gaming PC, and "a Python
script with no dependencies" is a much easier thing to run than one that needs
a virtualenv set up first. `urllib.request` is entirely adequate for moving a
2 MiB file every few minutes.

PLAN.md §8 is the API. The daemon speaks only HTTP -- it never parses a card
itself, because the server already does that with the verified parser and will
hand back the answer.
"""

from __future__ import annotations

import json
import logging
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass

log = logging.getLogger("slotsync.client")

USER_AGENT = "slotsync-dolphin/0.1"


class ServerError(RuntimeError):
    """The server said no."""

    def __init__(self, status: int, detail: str, payload: dict | None = None) -> None:
        super().__init__(detail)
        self.status = status
        self.detail = detail
        self.payload = payload or {}


class Conflict(ServerError):
    """409: the push was built on a version that is no longer head.

    Carries the server's head so the caller can show a real choice. Never
    resolved automatically -- PLAN.md §7.
    """

    def __init__(self, detail: str, payload: dict) -> None:
        super().__init__(409, detail, payload)
        self.head: int = payload.get("head", 0)
        self.head_sha256: str | None = payload.get("head_sha256")


@dataclass(frozen=True)
class PushResult:
    version: int
    outcome: str
    sha256: str
    size: int

    @property
    def created(self) -> bool:
        return self.outcome == "created"


@dataclass(frozen=True)
class PullResult:
    image: bytes
    version: int
    sha256: str


class Client:
    """Talks to one SlotSync server."""

    def __init__(self, base_url: str, token: str, *, timeout: float = 30.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.timeout = timeout

    # --- plumbing ---------------------------------------------------------

    def _request(
        self,
        method: str,
        path: str,
        *,
        params: dict | None = None,
        body: bytes | None = None,
        content_type: str | None = None,
    ):
        url = f"{self.base_url}{path}"
        if params:
            clean = {k: v for k, v in params.items() if v is not None}
            if clean:
                url = f"{url}?{urllib.parse.urlencode(clean)}"

        request = urllib.request.Request(url, data=body, method=method)
        request.add_header("Authorization", f"Bearer {self.token}")
        request.add_header("User-Agent", USER_AGENT)
        if content_type:
            request.add_header("Content-Type", content_type)

        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                # Header names are case-insensitive and uvicorn sends them
                # lowercased, so normalise rather than matching on the casing
                # the docs happen to use.
                headers = {k.lower(): v for k, v in response.headers.items()}
                return response.status, response.read(), headers
        except urllib.error.HTTPError as exc:
            raw = exc.read()
            payload = _maybe_json(raw)
            detail = payload.get("detail") or exc.reason or f"HTTP {exc.code}"
            if exc.code == 409:
                raise Conflict(detail, payload) from None
            raise ServerError(exc.code, detail, payload) from None
        except urllib.error.URLError as exc:
            raise ServerError(0, f"cannot reach {self.base_url}: {exc.reason}") from None

    def _json(self, method: str, path: str, **kwargs) -> dict:
        _, raw, _ = self._request(method, path, **kwargs)
        return _maybe_json(raw)

    # --- api --------------------------------------------------------------

    def healthy(self) -> bool:
        try:
            status, _, _ = self._request("GET", "/healthz")
            return status == 200
        except ServerError:
            return False

    def list_cards(self) -> list[dict]:
        return self._json("GET", "/api/cards").get("cards", [])

    def card(self, game_id: str, slot: str = "A") -> dict | None:
        """Detail including parsed saves, or None if the server has no card."""
        try:
            return self._json("GET", f"/api/cards/{game_id}/{slot}")
        except ServerError as exc:
            if exc.status == 404:
                return None
            raise

    def history(self, game_id: str, slot: str = "A") -> list[dict]:
        return self._json("GET", f"/api/cards/{game_id}/{slot}/versions").get(
            "versions", []
        )

    def pull(
        self, game_id: str, slot: str = "A", version: int | None = None
    ) -> PullResult:
        """Fetch a card image. `version` None means head."""
        name = "latest" if version is None else str(version)
        _, image, headers = self._request(
            "GET", f"/api/cards/{game_id}/{slot}/{name}.raw"
        )
        return PullResult(
            image=image,
            version=int(headers.get("x-slotsync-version", version or 0)),
            sha256=headers.get("x-slotsync-sha256", ""),
        )

    def push(
        self,
        game_id: str,
        slot: str,
        image: bytes,
        *,
        parent: int,
        device: int | None = None,
        note: str | None = None,
    ) -> PushResult:
        """Push a card image. Raises Conflict if `parent` is not head."""
        payload = self._json(
            "POST",
            f"/api/cards/{game_id}/{slot}",
            params={"parent": parent, "device": device, "note": note},
            body=image,
            content_type="application/octet-stream",
        )
        return PushResult(
            version=payload["version"],
            outcome=payload["outcome"],
            sha256=payload["sha256"],
            size=payload["size"],
        )

    def format(self, game_id: str, slot: str = "A", mbit: int = 16) -> PushResult:
        """Ask the server to create a blank card for a game it has never seen."""
        payload = self._json(
            "POST",
            f"/api/cards/{game_id}/{slot}/format",
            params={"mbit": mbit},
        )
        return PushResult(
            version=payload["version"],
            outcome=payload["outcome"],
            sha256=payload["sha256"],
            size=payload["size"],
        )

    def rollback(self, game_id: str, slot: str, version: int) -> dict:
        return self._json("POST", f"/api/cards/{game_id}/{slot}/rollback/{version}")


def _maybe_json(raw: bytes) -> dict:
    try:
        value = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return {}
    return value if isinstance(value, dict) else {}
