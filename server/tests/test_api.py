"""M1: the HTTP API -- PLAN.md section 8.

Done when a card round-trips byte-identical and a stale-parent push is a 409.
"""

from __future__ import annotations

import hashlib

import pytest
from fastapi.testclient import TestClient

from slotsync.app import create_app

from .conftest import TEST_TOKEN, make_card

#: Real, structurally valid card images -- these go through validation on
#: ingest just as a Dolphin daemon upload would.
CARD_A = make_card("Zelda Quest Log")
CARD_B = make_card("Mario Sunshine Save")

AUTH = {"Authorization": f"Bearer {TEST_TOKEN}"}


@pytest.fixture
def client(config) -> TestClient:
    with TestClient(create_app(config)) as test_client:
        yield test_client


def push(client, data, *, game="GALE01", slot="A", parent=0, **params):
    return client.post(
        f"/api/cards/{game}/{slot}",
        params={"parent": parent, **params},
        content=data,
        headers=AUTH,
    )


# --- auth -----------------------------------------------------------------


@pytest.mark.parametrize(
    "headers",
    [
        {},
        {"Authorization": "Bearer wrong"},
        {"Authorization": TEST_TOKEN},  # missing the scheme
        {"Authorization": "Basic " + TEST_TOKEN},
    ],
)
def test_api_requires_the_bearer_token(client, headers):
    response = client.get("/api/cards", headers=headers)
    assert response.status_code == 401
    assert response.headers["WWW-Authenticate"] == "Bearer"


def test_healthz_stays_open(client):
    assert client.get("/healthz").status_code == 200


# --- round trip -----------------------------------------------------------


def test_push_then_pull_is_byte_identical(client):
    """The M1 done condition."""
    created = push(client, CARD_A)
    assert created.status_code == 201
    assert created.json()["version"] == 1
    assert created.json()["outcome"] == "created"

    fetched = client.get("/api/cards/GALE01/A/latest.raw", headers=AUTH)
    assert fetched.status_code == 200
    assert fetched.content == CARD_A
    assert fetched.headers["content-type"] == "application/octet-stream"


def test_download_names_the_file_the_way_nintendont_does(client):
    push(client, CARD_A)
    response = client.get("/api/cards/GALE01/A/latest.raw", headers=AUTH)
    assert response.headers["content-disposition"] == 'attachment; filename="GALE01.raw"'


def test_download_carries_the_digest_as_an_etag(client):
    push(client, CARD_A)
    response = client.get("/api/cards/GALE01/A/latest.raw", headers=AUTH)
    assert response.headers["etag"].strip('"') == hashlib.sha256(CARD_A).hexdigest()


def test_a_specific_version_can_be_fetched(client):
    push(client, CARD_A)
    push(client, CARD_B, parent=1)

    assert client.get("/api/cards/GALE01/A/1.raw", headers=AUTH).content == CARD_A
    assert client.get("/api/cards/GALE01/A/2.raw", headers=AUTH).content == CARD_B


def test_slot_accepts_both_letters_and_numbers(client):
    push(client, CARD_A, slot="A")
    assert client.get("/api/cards/GALE01/0/latest.raw", headers=AUTH).content == CARD_A


# --- conflicts ------------------------------------------------------------


def test_stale_parent_push_returns_409_with_the_head(client):
    """The other half of the M1 done condition."""
    push(client, CARD_A)
    push(client, CARD_B, parent=1)

    response = push(client, make_card("Third Card"), parent=1)

    assert response.status_code == 409
    body = response.json()
    assert body["error"] == "conflict"
    assert body["head"] == 2
    assert body["head_sha256"] == hashlib.sha256(CARD_B).hexdigest()


def test_conflict_does_not_change_head(client):
    push(client, CARD_A)
    push(client, make_card("Nope"), parent=99)
    assert client.get("/api/cards/GALE01/A/latest.raw", headers=AUTH).content == CARD_A


def test_identical_repush_is_200_not_201(client):
    push(client, CARD_A)
    response = push(client, CARD_A)
    assert response.status_code == 200
    assert response.json() == {
        "game_id": "GALE01",
        "slot": 0,
        "version": 1,
        "outcome": "unchanged",
        "sha256": hashlib.sha256(CARD_A).hexdigest(),
        "size": len(CARD_A),
    }


def test_parent_is_required(client):
    """Defaulting it would make every stale push a silent overwrite."""
    response = client.post("/api/cards/GALE01/A", content=CARD_A, headers=AUTH)
    assert response.status_code == 400
    assert "parent" in response.json()["detail"]


# --- listings -------------------------------------------------------------


def test_cards_listing(client):
    push(client, CARD_A, game="GALE01", slot="A")
    push(client, CARD_B, game="GM4E01", slot="B")

    cards = client.get("/api/cards", headers=AUTH).json()["cards"]
    assert {(c["game_id"], c["slot_name"], c["head"]) for c in cards} == {
        ("GALE01", "A", 1),
        ("GM4E01", "B", 1),
    }


def test_card_detail(client):
    push(client, CARD_A)
    push(client, CARD_B, parent=1)

    body = client.get("/api/cards/GALE01/A", headers=AUTH).json()
    assert body["game_id"] == "GALE01"
    assert body["head"]["version"] == 2
    assert body["versions"] == 2


def test_version_history(client):
    push(client, CARD_A)
    push(client, CARD_B, parent=1, note="from dolphin")

    body = client.get("/api/cards/GALE01/A/versions", headers=AUTH).json()
    assert body["head"] == 2
    assert [v["version"] for v in body["versions"]] == [2, 1]
    assert body["versions"][0]["note"] == "from dolphin"
    assert body["versions"][0]["parent"] == 1


def test_devices_listing(client):
    push(client, CARD_A, device=0xDEADBEEFCAFEBABE)
    devices = client.get("/api/devices", headers=AUTH).json()["devices"]
    assert [d["device_id"] for d in devices] == [0xDEADBEEFCAFEBABE]
    assert devices[0]["kind"] == "dolphin"


# --- rollback -------------------------------------------------------------


def test_rollback_restores_an_older_image_as_a_new_version(client):
    push(client, CARD_A)
    push(client, CARD_B, parent=1)

    response = client.post("/api/cards/GALE01/A/rollback/1", headers=AUTH)

    assert response.status_code == 200
    assert response.json()["version"] == 3
    assert response.json()["restored_from"] == 1
    assert client.get("/api/cards/GALE01/A/latest.raw", headers=AUTH).content == CARD_A


def test_rollback_to_unknown_version_is_404(client):
    push(client, CARD_A)
    assert client.post("/api/cards/GALE01/A/rollback/9", headers=AUTH).status_code == 404


# --- errors ---------------------------------------------------------------


def test_unknown_card_is_404(client):
    response = client.get("/api/cards/ZZZZ99/A/latest.raw", headers=AUTH)
    assert response.status_code == 404
    assert response.json()["error"] == "not_found"


def test_bad_game_id_is_400(client):
    assert push(client, CARD_A, game="way-too-long").status_code == 400


def test_bad_slot_is_400(client):
    assert push(client, CARD_A, slot="Z").status_code == 400


def test_empty_body_is_400(client):
    assert push(client, b"").status_code == 400


def test_oversized_body_is_413(client, config):
    response = push(client, b"x" * (config.max_card_bytes + 1))
    assert response.status_code == 413
    assert response.json()["error"] == "too_large"


def test_non_numeric_version_is_400(client):
    push(client, CARD_A)
    assert client.get("/api/cards/GALE01/A/nope.raw", headers=AUTH).status_code == 400
