"""M0: `docker compose up` serves GET /healthz -> 200."""

from __future__ import annotations

from fastapi.testclient import TestClient

from slotsync import __version__
from slotsync.app import create_app


def test_healthz_returns_200_without_auth(config):
    with TestClient(create_app(config)) as client:
        response = client.get("/healthz")

    assert response.status_code == 200
    assert response.json() == {"status": "ok", "version": __version__}


def test_startup_creates_the_data_directories(config):
    create_app(config)
    assert config.data_dir.is_dir()
    assert config.blobs_dir.is_dir()
