"""Content-addressed blob store.

    /data/blobs/<sha256[0:2]>/<sha256>.raw

Cards dedupe for free: the same image pushed from a Wii and from Dolphin lands
on the same path and is stored once. PLAN.md section 6.

Cards top out at 16 MiB, so blobs are handled whole in memory. Streaming would
buy nothing here and would complicate the hashing.
"""

from __future__ import annotations

import hashlib
import logging
import os
import tempfile
from pathlib import Path

log = logging.getLogger("slotsync.blobs")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class BlobStore:
    """Immutable, content-addressed storage for whole card images."""

    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def path_for(self, digest: str) -> Path:
        """Path a blob with this digest occupies, whether or not it exists."""
        if len(digest) != 64 or not all(c in "0123456789abcdef" for c in digest):
            raise ValueError(f"not a lowercase sha256 hex digest: {digest!r}")
        return self.root / digest[:2] / f"{digest}.raw"

    def exists(self, digest: str) -> bool:
        return self.path_for(digest).is_file()

    def put(self, data: bytes) -> str:
        """Store `data`, returning its digest. Idempotent."""
        digest = sha256_hex(data)
        target = self.path_for(digest)

        if target.is_file():
            log.debug("blob already present", extra={"sha256": digest})
            return digest

        target.parent.mkdir(parents=True, exist_ok=True)

        # Write to a sibling temp file and rename, so a crash mid-write can
        # never leave a truncated file sitting at a name that claims to be a
        # verified digest.
        fd, tmp = tempfile.mkstemp(dir=target.parent, suffix=".tmp")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(tmp, target)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise

        log.info("blob stored", extra={"sha256": digest, "size": len(data)})
        return digest

    def get(self, digest: str) -> bytes:
        """Read a blob. Raises FileNotFoundError if it is not stored."""
        return self.path_for(digest).read_bytes()

    def size(self, digest: str) -> int:
        return self.path_for(digest).stat().st_size

    def delete(self, digest: str) -> bool:
        """Remove a blob. Only ever called by explicit admin pruning; see
        PLAN.md section 6 -- versions are not deleted by default."""
        path = self.path_for(digest)
        try:
            path.unlink()
        except FileNotFoundError:
            return False
        log.info("blob deleted", extra={"sha256": digest})
        return True
