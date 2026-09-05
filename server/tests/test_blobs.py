"""M1: the content-addressed blob store."""

from __future__ import annotations

import hashlib

import pytest

from slotsync.blobs import BlobStore, sha256_hex


@pytest.fixture
def blobs(tmp_path) -> BlobStore:
    return BlobStore(tmp_path / "blobs")


def test_put_returns_the_digest_and_get_round_trips(blobs):
    data = bytes(range(256)) * 32
    digest = blobs.put(data)
    assert digest == hashlib.sha256(data).hexdigest()
    assert blobs.get(digest) == data


def test_layout_is_two_character_fanout(blobs):
    digest = blobs.put(b"hello")
    path = blobs.path_for(digest)
    assert path.parent.name == digest[:2]
    assert path.name == f"{digest}.raw"
    assert path.is_file()


def test_put_is_idempotent_and_stores_once(blobs):
    blobs.put(b"hello")
    blobs.put(b"hello")
    assert len(list(blobs.root.rglob("*.raw"))) == 1


def test_exists_and_size(blobs):
    digest = blobs.put(b"hello")
    assert blobs.exists(digest)
    assert blobs.size(digest) == 5
    assert not blobs.exists(sha256_hex(b"absent"))


def test_missing_blob_raises(blobs):
    with pytest.raises(FileNotFoundError):
        blobs.get(sha256_hex(b"absent"))


@pytest.mark.parametrize(
    "bad",
    [
        "",
        "short",
        "../../etc/passwd",
        "Z" * 64,
        "A" * 64,  # uppercase hex is not the canonical form we write
        "a" * 63,
    ],
)
def test_path_for_refuses_anything_that_is_not_a_digest(blobs, bad):
    """The digest becomes a filesystem path, so it is validated, not trusted."""
    with pytest.raises(ValueError):
        blobs.path_for(bad)


def test_no_temp_files_are_left_behind(blobs):
    blobs.put(b"hello")
    assert list(blobs.root.rglob("*.tmp")) == []


def test_delete_removes_and_reports(blobs):
    digest = blobs.put(b"hello")
    assert blobs.delete(digest) is True
    assert blobs.delete(digest) is False
    assert not blobs.exists(digest)
