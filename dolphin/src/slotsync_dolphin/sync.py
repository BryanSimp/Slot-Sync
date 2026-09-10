"""Pull, push, and the play cycle.

This is where the per-game card model from PLAN.md §5 actually happens: pull a
game's card, point Dolphin at it, let the game run, push the result back.

The one rule everything here bends around is §7. A push carries the version it
was derived from, and a stale one is refused rather than merged. Nothing in
this module resolves a conflict on its own; it reports one and stops.
"""

from __future__ import annotations

import logging
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from .cards import CardDirectory, sha256_hex
from .client import Client, Conflict, ServerError
from .config import Config
from .dolphin import DolphinConfig, DolphinPaths, find_executable, is_running

log = logging.getLogger("slotsync.sync")


class SyncError(RuntimeError):
    """The operation cannot proceed."""


@dataclass
class PullOutcome:
    game_id: str
    slot: str
    version: int
    #: "pulled" new bytes, "current" already matched, "formatted" created blank.
    action: str
    path: Path


@dataclass
class PushOutcome:
    game_id: str
    slot: str
    version: int
    #: "pushed", "unchanged" (identical to head), or "nothing" (no local edits).
    action: str


class Syncer:
    """Everything the CLI does, minus the argument parsing."""

    def __init__(self, config: Config) -> None:
        self.config = config
        self.client = Client(config.server, config.require_token())
        self.cards = CardDirectory(config.cards_dir)

    # --- dolphin ----------------------------------------------------------

    def dolphin_config(self) -> DolphinConfig:
        from .dolphin import default_user_dir

        user_dir = self.config.user_dir or default_user_dir()
        if user_dir is None:
            raise SyncError(
                "cannot find Dolphin's user directory. Pass --user-dir with the "
                "folder containing Config/Dolphin.ini."
            )
        return DolphinConfig(DolphinPaths(user_dir=Path(user_dir)))

    def point_dolphin_at(self, game_id: str, slot: str, *, force: bool = False) -> bool:
        """Repoint Dolphin's slot at this game's card.

        Refuses while Dolphin is running: it rewrites Dolphin.ini on exit from
        the settings it loaded at startup, so an edit made underneath a live
        instance is silently reverted and the game plays against the wrong card.
        """
        if is_running() and not force:
            raise SyncError(
                "Dolphin is running. It rewrites Dolphin.ini when it exits, so "
                "changing the memory card now would be undone. Close Dolphin "
                "and try again, or pass --force if you know the running "
                "instance is not the one you are configuring."
            )
        if is_running() and force:
            log.warning(
                "Dolphin is running and --force was given; it may overwrite "
                "this change to Dolphin.ini when it exits"
            )

        index = 1 if slot.upper() == "B" else 0
        card = self.cards.path_for(game_id, slot)
        return self.dolphin_config().point_at(card, index)

    # --- pull -------------------------------------------------------------

    def pull(
        self, game_id: str, slot: str | None = None, *, create: bool = True
    ) -> PullOutcome:
        """Bring the local card up to the server's head.

        Refuses to clobber local edits: if the file on disk differs from what
        we last agreed with the server, that is unpushed play and overwriting it
        is the failure this project exists to prevent.
        """
        slot = (slot or self.config.slot).upper()
        game_id = game_id.upper()

        detail = self.client.card(game_id, slot)
        if detail is None:
            if not create:
                raise SyncError(f"the server has no card for {game_id} slot {slot}")
            log.info(
                "server has no card for this game; formatting a blank one",
                extra={"game_id": game_id, "slot": slot, "mbit": self.config.mbit},
            )
            self.client.format(game_id, slot, self.config.mbit)
            detail = self.client.card(game_id, slot)

        head_version = detail["head"]["version"]
        head_sha = detail["head"]["sha256"]

        if self.cards.has_local_changes(game_id, slot):
            local = self.cards.local_digest(game_id, slot)
            if local != head_sha:
                raise SyncError(
                    f"{game_id} slot {slot} has local changes that are not on the "
                    f"server. Push them first, or delete "
                    f"{self.cards.path_for(game_id, slot)} to discard them."
                )

        if self.cards.local_digest(game_id, slot) == head_sha:
            self.cards.remember(game_id, slot, head_version, head_sha)
            return PullOutcome(
                game_id, slot, head_version, "current", self.cards.path_for(game_id, slot)
            )

        result = self.client.pull(game_id, slot)
        if sha256_hex(result.image) != head_sha:
            raise SyncError(
                f"the image the server sent does not match the digest it "
                f"advertised for version {head_version}"
            )

        path = self.cards.write(game_id, slot, result.image)
        self.cards.remember(game_id, slot, result.version, result.sha256 or head_sha)
        log.info(
            "pulled",
            extra={"game_id": game_id, "slot": slot, "version": result.version},
        )
        return PullOutcome(game_id, slot, result.version, "pulled", path)

    # --- push -------------------------------------------------------------

    def push(self, game_id: str, slot: str | None = None) -> PushOutcome:
        """Send local changes, based on the version we last pulled."""
        slot = (slot or self.config.slot).upper()
        game_id = game_id.upper()

        if not self.cards.exists(game_id, slot):
            raise SyncError(f"no local card at {self.cards.path_for(game_id, slot)}")

        state = self.cards.state(game_id, slot)
        image = self.cards.read(game_id, slot)
        digest = sha256_hex(image)

        if digest == state.sha256:
            return PushOutcome(game_id, slot, state.version, "nothing")

        result = self.client.push(
            game_id,
            slot,
            image,
            parent=state.version,
            device=self.config.device,
            note="from dolphin",
        )
        self.cards.remember(game_id, slot, result.version, result.sha256)
        log.info(
            "pushed",
            extra={
                "game_id": game_id,
                "slot": slot,
                "version": result.version,
                "outcome": result.outcome,
            },
        )
        return PushOutcome(
            game_id,
            slot,
            result.version,
            "pushed" if result.created else "unchanged",
        )

    # --- play -------------------------------------------------------------

    def play(
        self,
        game_id: str,
        game_path: Path | None = None,
        slot: str | None = None,
        *,
        launch: bool = True,
        force: bool = False,
    ) -> tuple[PullOutcome, PushOutcome | None]:
        """The seamless path: pull, point Dolphin, run, push.

        The push happens in a finally block. A crashed emulator is exactly when
        you most want the save that did get written.
        """
        slot = (slot or self.config.slot).upper()

        pulled = self.pull(game_id, slot)
        self.point_dolphin_at(game_id, slot, force=force)

        if not launch:
            return pulled, None

        executable = self.config.dolphin_exe or find_executable()
        if executable is None:
            raise SyncError(
                "cannot find the Dolphin executable. Pass --dolphin-exe, or run "
                "`use` instead and start Dolphin yourself."
            )

        command = [str(executable)]
        if game_path is not None:
            command += ["--exec", str(game_path)]

        log.info("launching dolphin", extra={"command": " ".join(command)})
        try:
            subprocess.run(command, check=False)
        finally:
            try:
                pushed = self.push(game_id, slot)
            except Conflict:
                raise
            except (ServerError, SyncError) as exc:
                log.error(
                    "could not push after play; the card is still on disk",
                    extra={
                        "detail": str(exc),
                        "path": str(self.cards.path_for(game_id, slot)),
                    },
                )
                raise

        return pulled, pushed

    # --- watch ------------------------------------------------------------

    def watch_once(self) -> list[PushOutcome]:
        """Push every local card that has settled since it last changed.

        Polling rather than a filesystem-watch library: this component has no
        dependencies, and a memory card changes every few minutes at most.
        """
        outcomes = []
        now = time.time()

        for game_id, slot in self.cards.known_games():
            if not self.cards.has_local_changes(game_id, slot):
                continue

            path = self.cards.path_for(game_id, slot)
            # Wait for the file to stop moving. Dolphin flushes a card in
            # pieces, and pushing a half-written one wastes a version.
            if now - path.stat().st_mtime < self.config.settle_seconds:
                continue

            try:
                outcomes.append(self.push(game_id, slot))
            except Conflict as exc:
                log.warning(
                    "conflict: another device moved this card on; not overwriting",
                    extra={"game_id": game_id, "slot": slot, "head": exc.head},
                )
            except (ServerError, SyncError) as exc:
                log.error("push failed", extra={"game_id": game_id, "detail": str(exc)})
        return outcomes

    def current_game(self, slot: str | None = None) -> str | None:
        """The game Dolphin's memory card path names, if it is one of ours.

        Dolphin has exactly one `MemcardAPath`, so this is the only game it can
        play correctly right now. None if it points somewhere we do not manage,
        which is the case worth telling someone about rather than guessing at.
        """
        slot = (slot or self.config.slot).upper()
        try:
            dolphin = self.dolphin_config()
        except SyncError:
            return None

        path = dolphin.memcard_path(1 if slot == "B" else 0)
        if path is None:
            return None
        try:
            path = path.resolve()
        except OSError:
            return None
        if path.parent != self.cards.root.resolve():
            return None

        for game_id, known_slot in self.cards.known_games():
            if known_slot != slot:
                continue
            if self.cards.path_for(game_id, known_slot).resolve() == path:
                return game_id
        return None

    def pull_idle_cards(self) -> list[PullOutcome]:
        """Bring local cards up to the server's head, when it is safe to.

        Only while Dolphin is closed. It keeps the card in memory and writes it
        back out, so a card replaced underneath a running instance is undone the
        next time the game saves -- and the save that replaced it is lost.

        A card with unpushed local play is left alone: `pull` refuses it, which
        is a conflict for a human rather than something to resolve here.
        """
        outcomes = []

        if is_running():
            return outcomes

        for game_id, slot in self.cards.known_games():
            try:
                detail = self.client.card(game_id, slot)
            except ServerError as exc:
                log.error(
                    "head query failed",
                    extra={"game_id": game_id, "detail": str(exc)},
                )
                continue
            if detail is None:
                continue
            if detail["head"]["version"] <= self.cards.state(game_id, slot).version:
                continue

            try:
                outcomes.append(self.pull(game_id, slot, create=False))
            except (ServerError, SyncError) as exc:
                log.warning(
                    "not pulling",
                    extra={"game_id": game_id, "slot": slot, "detail": str(exc)},
                )
        return outcomes

    def watch(self) -> None:
        """Poll forever, both directions. Ctrl-C to stop."""
        log.info(
            "watching for changes",
            extra={
                "cards_dir": str(self.cards.root),
                "interval": self.config.poll_interval,
            },
        )
        next_pull = 0.0
        was_running = False

        while True:
            self.watch_once()

            # Dolphin just let go of the cards. Do not wait out the rest of the
            # interval: this is the moment someone is most likely to go and
            # start it again, and starting it against a stale card is how a
            # save gets forked.
            running = is_running()
            just_closed = was_running and not running
            was_running = running

            now = time.monotonic()
            if just_closed or now >= next_pull:
                next_pull = now + self.config.pull_interval
                self.pull_idle_cards()

            time.sleep(self.config.poll_interval)
