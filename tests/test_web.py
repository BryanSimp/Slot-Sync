"""M3: the web UI -- PLAN.md section 9.

Done when the whole push / conflict / rollback cycle is drivable from a
browser. The conflict resolution itself lives in the page's JavaScript, so what
is asserted here is that the server gives that JavaScript everything it needs:
the current head on the page, and a 409 carrying the new head.
"""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from slotsync.api import COOKIE_NAME
from slotsync.app import create_app

from .conftest import TEST_TOKEN, make_card

CARD_A = make_card("Zelda Quest Log")
CARD_B = make_card("Mario Sunshine Save")

AUTH = {"Authorization": f"Bearer {TEST_TOKEN}"}


@pytest.fixture
def client(config) -> TestClient:
    with TestClient(create_app(config), follow_redirects=False) as test_client:
        yield test_client


@pytest.fixture
def signed_in(client) -> TestClient:
    client.cookies.set(COOKIE_NAME, TEST_TOKEN)
    return client


def seed(client, data=CARD_A, *, game="GALE01", slot="A", parent=0, **params):
    response = client.post(
        f"/api/cards/{game}/{slot}",
        params={"parent": parent, **params},
        content=data,
        headers=AUTH,
    )
    return response


# --- auth -----------------------------------------------------------------


def test_signed_out_visitor_is_sent_to_the_login_page(client):
    response = client.get("/")
    assert response.status_code == 303
    assert "/login" in response.headers["location"]


def test_login_page_renders(client):
    response = client.get("/login")
    assert response.status_code == 200
    assert "SLOTSYNC_TOKEN" in response.text


def test_correct_token_sets_the_cookie(client):
    response = client.post("/login", data={"token": TEST_TOKEN, "next": "/"})
    assert response.status_code == 303
    assert response.headers["location"] == "/"

    cookie = response.headers["set-cookie"]
    assert COOKIE_NAME in cookie
    assert "HttpOnly" in cookie
    # SameSite=Lax is what stops a cross-site form POST riding on this cookie.
    assert "SameSite=lax" in cookie.lower().replace("samesite=lax", "SameSite=lax")


def test_wrong_token_is_rejected(client):
    response = client.post("/login", data={"token": "nope", "next": "/"})
    assert response.status_code == 401
    assert "does not match" in response.text
    assert COOKIE_NAME not in response.headers.get("set-cookie", "")


def test_login_will_not_redirect_off_site(client):
    """`next` comes from the query string, so it must not become an open redirect."""
    response = client.post(
        "/login", data={"token": TEST_TOKEN, "next": "https://evil.example/"}
    )
    assert response.headers["location"] == "/"


def test_login_preserves_a_local_next(client):
    response = client.post(
        "/login", data={"token": TEST_TOKEN, "next": "/cards/GALE01/A"}
    )
    assert response.headers["location"] == "/cards/GALE01/A"


def test_the_cookie_also_authenticates_the_api(signed_in):
    """The download links are plain <a href>, which can only carry a cookie."""
    seed(signed_in)
    response = signed_in.get("/api/cards/GALE01/A/latest.raw")
    assert response.status_code == 200
    assert response.content == CARD_A


def test_logout_clears_the_cookie(signed_in):
    response = signed_in.post("/logout")
    assert response.status_code == 303
    assert (
        'slotsync_token=""' in response.headers["set-cookie"]
        or "slotsync_token=;" in response.headers["set-cookie"]
    )


# --- pages ----------------------------------------------------------------


def test_empty_card_list_explains_what_to_do(signed_in):
    response = signed_in.get("/")
    assert response.status_code == 200
    assert "Nothing stored yet" in response.text
    assert "curl" in response.text


def test_card_list_shows_every_card(signed_in):
    seed(signed_in, CARD_A, game="GALE01", slot="A")
    seed(signed_in, CARD_B, game="GM4E01", slot="B")

    response = signed_in.get("/")
    assert "GALE01" in response.text
    assert "GM4E01" in response.text


def test_card_detail_shows_real_save_names_and_block_counts(signed_in):
    """PLAN.md section 9: real save names, not just filenames."""
    seed(signed_in)
    response = signed_in.get("/cards/GALE01/A")

    assert response.status_code == 200
    assert "Zelda Quest Log" in response.text  # the parsed comment, not a filename
    assert "Saves on this card" in response.text
    assert "16 Mbit" in response.text or "4 Mbit" in response.text


def test_card_detail_lists_version_history_with_downloads_and_rollback(signed_in):
    seed(signed_in, CARD_A)
    seed(signed_in, CARD_B, parent=1)

    response = signed_in.get("/cards/GALE01/A")

    assert "Version history" in response.text
    assert "/api/cards/GALE01/A/1.raw" in response.text  # download per version
    assert "/cards/GALE01/A/rollback/1" in response.text  # restore per version
    # ...but not for head: there is nothing to roll back to.
    assert "/cards/GALE01/A/rollback/2" not in response.text


def test_detail_page_publishes_the_head_the_uploader_should_use(signed_in):
    """The push form sends `parent` from this attribute; a wrong value here
    would turn every push into a spurious conflict."""
    seed(signed_in, CARD_A)
    seed(signed_in, CARD_B, parent=1)

    response = signed_in.get("/cards/GALE01/A")
    assert 'data-head="2"' in response.text


def test_damaged_card_shows_a_warning_banner(signed_in):
    damaged = bytearray(CARD_A)
    damaged[1 * 8192 + 0x40] ^= 0xFF  # break the primary directory copy
    seed(signed_in, bytes(damaged))

    response = signed_in.get("/cards/GALE01/A")
    assert "Stored with damage" in response.text
    assert "primary copy of the directory is damaged" in response.text


def test_unknown_card_detail_is_404(signed_in):
    assert signed_in.get("/cards/ZZZZ99/A").status_code == 404


# --- the cycle ------------------------------------------------------------


def test_a_stale_push_returns_the_head_the_banner_needs(signed_in):
    """The conflict banner names both versions, so the 409 must carry head."""
    seed(signed_in, CARD_A)
    seed(signed_in, CARD_B, parent=1)

    response = seed(signed_in, make_card("Third"), parent=1)

    assert response.status_code == 409
    assert response.json()["head"] == 2


def test_keep_mine_is_a_normal_push_onto_the_new_head(signed_in):
    """What the "Keep mine" button does: re-push against the head from the 409.
    The losing version stays in history rather than being discarded."""
    seed(signed_in, CARD_A)
    seed(signed_in, CARD_B, parent=1)

    mine = make_card("Mine")
    assert seed(signed_in, mine, parent=1).status_code == 409
    assert seed(signed_in, mine, parent=2).status_code == 201

    assert signed_in.get("/api/cards/GALE01/A/latest.raw").content == mine
    assert signed_in.get("/api/cards/GALE01/A/2.raw").content == CARD_B


def test_rollback_redirects_back_to_the_page_not_the_json_api(signed_in):
    """Regression: both routers defined a `card_detail`, and url_for resolved
    to the API's, so Restore dumped the visitor into raw JSON."""
    seed(signed_in, CARD_A)
    seed(signed_in, CARD_B, parent=1)

    response = signed_in.post("/cards/GALE01/A/rollback/1")

    assert response.status_code == 303
    location = response.headers["location"]
    assert location.endswith("/cards/GALE01/A")
    assert "/api/" not in location


def test_rollback_from_the_browser_restores_the_image(signed_in):
    seed(signed_in, CARD_A)
    seed(signed_in, CARD_B, parent=1)

    signed_in.post("/cards/GALE01/A/rollback/1")

    assert signed_in.get("/api/cards/GALE01/A/latest.raw").content == CARD_A
    # History is intact: rollback appended rather than truncating.
    assert signed_in.get("/api/cards/GALE01/A/2.raw").content == CARD_B


def test_rollback_requires_a_session(client):
    seed(client, CARD_A)
    response = client.post("/cards/GALE01/A/rollback/1")
    assert response.status_code == 303
    assert "/login" in response.headers["location"]
