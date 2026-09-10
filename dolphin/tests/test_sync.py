"""The daemon against a real server.

These run the actual SlotSync app in a thread, so they cover the wire between
the two components rather than a mock of it.
"""

from __future__ import annotations

import pytest

from slotsync_dolphin.cards import CardDirectory, sha256_hex
from slotsync_dolphin.client import Conflict
from slotsync_dolphin.dolphin import (
    EXI_MEMORY_CARD,
    DolphinConfig,
    DolphinPaths,
)
from slotsync_dolphin.sync import Syncer, SyncError

from .conftest import make_card


@pytest.fixture
def syncer(config) -> Syncer:
    return Syncer(config)


def unique(name: str) -> str:
    """Distinct game ids per test: the server fixture is session-scoped."""
    import hashlib

    return "G" + hashlib.sha256(name.encode()).hexdigest()[:5].upper()


# --- pull -----------------------------------------------------------------


def test_pulling_an_unknown_game_formats_a_blank_card(syncer):
    """A game with no history needs a card made before it can be played."""
    game = unique("format")
    outcome = syncer.pull(game)

    assert outcome.action == "pulled"
    assert outcome.version == 1
    assert outcome.path.is_file()
    assert outcome.path.stat().st_size == 4 * 16 * 8192  # the 4 Mbit test default

    detail = syncer.client.card(game)
    assert detail["saves"] == []


def test_pulling_twice_is_a_no_op(syncer):
    game = unique("twice")
    syncer.pull(game)
    assert syncer.pull(game).action == "current"


def test_pull_verifies_the_digest_it_was_promised(syncer, monkeypatch):
    """A truncated download must not become a card we then push back."""
    game = unique("digest")
    syncer.pull(game)

    from slotsync_dolphin.client import PullResult

    monkeypatch.setattr(
        syncer.client,
        "pull",
        lambda *a, **k: PullResult(image=b"not the card", version=1, sha256="x"),
    )
    syncer.cards.path_for(game, "A").unlink()
    syncer.cards.remember(game, "A", 0, "")

    with pytest.raises(SyncError, match="does not match the digest"):
        syncer.pull(game)


def test_pull_refuses_to_clobber_unpushed_play(syncer):
    """Overwriting local changes is the exact failure this project prevents."""
    game = unique("clobber")
    syncer.pull(game)
    syncer.cards.write(game, "A", make_card("Unpushed Progress"))

    with pytest.raises(SyncError, match="local changes"):
        syncer.pull(game)


# --- push -----------------------------------------------------------------


def test_push_sends_local_changes(syncer):
    game = unique("push")
    syncer.pull(game)
    syncer.cards.write(game, "A", make_card("Zelda Save"))

    outcome = syncer.push(game)

    assert outcome.action == "pushed"
    assert outcome.version == 2
    assert syncer.client.card(game)["saves"][0]["display_name"] == "Zelda Save"


def test_pushing_with_nothing_to_send_does_nothing(syncer):
    game = unique("nothing")
    syncer.pull(game)
    assert syncer.push(game).action == "nothing"


def test_push_remembers_the_new_version(syncer):
    """Without this the next push would claim a stale parent and conflict."""
    game = unique("remember")
    syncer.pull(game)
    syncer.cards.write(game, "A", make_card("First"))
    syncer.push(game)

    assert syncer.cards.state(game, "A").version == 2

    syncer.cards.write(game, "A", make_card("Second"))
    assert syncer.push(game).version == 3


def test_pushing_a_card_that_does_not_exist_locally(syncer):
    with pytest.raises(SyncError, match="no local card"):
        syncer.push(unique("absent"))


def test_a_stale_push_raises_conflict_and_changes_nothing(syncer):
    """Another device moved head on while this one was playing."""
    game = unique("conflict")
    syncer.pull(game)

    # Someone else pushes.
    theirs = make_card("Their Progress")
    syncer.client.push(game, "A", theirs, parent=1)

    # We push against the version we pulled.
    syncer.cards.write(game, "A", make_card("Our Progress"))
    with pytest.raises(Conflict) as caught:
        syncer.push(game)

    assert caught.value.head == 2
    # Head is still theirs; nothing was overwritten.
    assert syncer.client.pull(game, "A").image == theirs
    # ...and our card is still on disk for the human to decide about.
    assert syncer.cards.exists(game, "A")


def test_pushing_the_same_bytes_as_head_is_free(syncer):
    """Idempotent retries must cost nothing -- PLAN.md section 7."""
    game = unique("idempotent")
    syncer.pull(game)
    image = make_card("Same")
    syncer.cards.write(game, "A", image)
    first = syncer.push(game)

    # Pretend the reply was lost and the state never updated.
    syncer.cards.remember(game, "A", first.version - 1, "")
    assert syncer.push(game).action == "unchanged"


# --- dolphin wiring -------------------------------------------------------


def test_play_without_launching_points_dolphin_at_the_card(syncer, config):
    game = unique("play")
    pulled, pushed = syncer.play(game, launch=False)

    assert pushed is None
    dolphin = DolphinConfig(DolphinPaths(user_dir=config.user_dir))
    assert dolphin.memcard_path(0) == pulled.path.resolve()
    assert dolphin.slot_device(0) == EXI_MEMORY_CARD


def test_pointing_dolphin_enables_a_slot_that_was_switched_off(syncer, config):
    """SlotA = 255 means nothing plugged in; the path alone would be ignored
    and the game would see no card at all."""
    dolphin = DolphinConfig(DolphinPaths(user_dir=config.user_dir))
    assert dolphin.slot_device(0) == 255

    game = unique("slotoff")
    syncer.pull(game)
    syncer.point_dolphin_at(game, "A")

    after = DolphinConfig(DolphinPaths(user_dir=config.user_dir))
    assert after.slot_device(0) == EXI_MEMORY_CARD


#: What a real Dolphin writes for `SlotA` when the slot is set to GCI-folder
#: mode. A literal on purpose: the constants in `slotsync_dolphin.dolphin` are
#: what this test exists to check, so expressing it in terms of them would make
#: the test agree with whatever they happen to say.
REAL_DOLPHIN_GCI_FOLDER = 8


def test_pointing_dolphin_takes_a_slot_out_of_gci_folder_mode(syncer, config):
    """A slot in GCI-folder mode ignores MemcardAPath and keeps saving into
    GC/<region>/Card A/*.gci.

    This is worse than a slot switched off, because everything looks like it
    worked: the path is written, Dolphin loads, the game saves -- into the
    folder. The card the daemon watches never changes, so `push` reports no
    local changes forever and the play is quietly stranded. Caught on a real
    installation after a session was lost to it.
    """
    ini = config.user_dir / "Config" / "Dolphin.ini"
    ini.write_text(
        ini.read_text(encoding="utf-8").replace(
            "SlotA = 255", f"SlotA = {REAL_DOLPHIN_GCI_FOLDER}"
        ),
        encoding="utf-8",
    )

    game = unique("gcifolder")
    syncer.pull(game)
    syncer.point_dolphin_at(game, "A")

    after = DolphinConfig(DolphinPaths(user_dir=config.user_dir))
    assert after.slot_device(0) != REAL_DOLPHIN_GCI_FOLDER
    assert after.slot_device(0) == EXI_MEMORY_CARD
    assert after.memcard_path(0) == syncer.cards.path_for(game, "A").resolve()


def test_slot_b_uses_its_own_keys_and_filename(syncer, config):
    game = unique("slotb")
    syncer.pull(game, "B")
    syncer.point_dolphin_at(game, "B")

    dolphin = DolphinConfig(DolphinPaths(user_dir=config.user_dir))
    assert dolphin.memcard_path(1).name == f"{game}-B.raw"
    assert dolphin.slot_device(1) == EXI_MEMORY_CARD


# --- watch ----------------------------------------------------------------


def test_watch_pushes_settled_cards(syncer):
    game = unique("watch")
    syncer.pull(game)
    syncer.cards.write(game, "A", make_card("Watched"))

    outcomes = syncer.watch_once()

    assert [o.game_id for o in outcomes] == [game]
    assert outcomes[0].action == "pushed"


def test_watch_ignores_cards_with_no_changes(syncer):
    game = unique("watch-quiet")
    syncer.pull(game)
    assert syncer.watch_once() == []


def test_watch_pulls_what_the_server_moved_on(syncer, monkeypatch):
    """The other half of unattended sync: a console pushed while the PC sat
    idle, and the card should come down without anyone running anything."""
    game = unique("idlepull")
    syncer.pull(game)
    ahead = syncer.cards.read(game, "A")[:]
    syncer.cards.write(game, "A", ahead[:-1] + bytes([ahead[-1] ^ 0xFF]))
    pushed = syncer.push(game, "A")

    # Rewind the local side to look like a PC that has not caught up yet.
    syncer.cards.write(game, "A", ahead)
    syncer.cards.remember(game, "A", pushed.version - 1, sha256_hex(ahead))

    monkeypatch.setattr("slotsync_dolphin.sync.is_running", lambda: False)
    outcomes = syncer.pull_idle_cards()

    assert [o.game_id for o in outcomes] == [game]
    assert syncer.cards.state(game, "A").version == pushed.version


def test_watch_never_pulls_under_a_running_dolphin(syncer, monkeypatch):
    """Dolphin holds the card in memory and writes it back out. A card replaced
    underneath it is undone at the next in-game save, and whatever was pulled is
    lost with it -- so this must not happen however far behind the card is."""
    game = unique("runningpull")
    syncer.pull(game)
    original = syncer.cards.read(game, "A")[:]
    syncer.cards.write(game, "A", original[:-1] + bytes([original[-1] ^ 0xFF]))
    pushed = syncer.push(game, "A")

    syncer.cards.write(game, "A", original)
    syncer.cards.remember(game, "A", pushed.version - 1, sha256_hex(original))

    monkeypatch.setattr("slotsync_dolphin.sync.is_running", lambda: True)
    assert syncer.pull_idle_cards() == []
    assert syncer.cards.read(game, "A") == original


def test_watch_survives_a_conflict_without_overwriting(syncer):
    """One card conflicting must not stop the others, or overwrite anything."""
    game = unique("watch-conflict")
    syncer.pull(game)
    theirs = make_card("Theirs")
    syncer.client.push(game, "A", theirs, parent=1)
    syncer.cards.write(game, "A", make_card("Ours"))

    assert syncer.watch_once() == []
    assert syncer.client.pull(game, "A").image == theirs


# --- card directory -------------------------------------------------------


def test_cards_are_named_the_way_nintendont_names_them(tmp_path):
    """So a file can move between an SD card and here without renaming."""
    cards = CardDirectory(tmp_path)
    assert cards.path_for("GALE01", "A").name == "GALE01.raw"
    assert cards.path_for("gale01", "A").name == "GALE01.raw"
    assert cards.path_for("GALE01", "B").name == "GALE01-B.raw"


def test_known_games_reads_both_slots_back(tmp_path):
    cards = CardDirectory(tmp_path)
    cards.write("GALE01", "A", b"a")
    cards.write("GM4E01", "B", b"b")
    assert sorted(cards.known_games()) == [("GALE01", "A"), ("GM4E01", "B")]


def test_writes_are_atomic_and_leave_no_temp_files(tmp_path):
    cards = CardDirectory(tmp_path)
    cards.write("GALE01", "A", b"x" * 4096)
    assert list(tmp_path.glob("*.tmp")) == []


def test_local_changes_are_detected_by_content_not_timestamp(tmp_path):
    cards = CardDirectory(tmp_path)
    cards.write("GALE01", "A", b"one")
    cards.remember("GALE01", "A", 1, sha256_hex(b"one"))
    assert not cards.has_local_changes("GALE01", "A")

    cards.write("GALE01", "A", b"two")
    assert cards.has_local_changes("GALE01", "A")


def test_state_survives_a_new_directory_object(tmp_path):
    CardDirectory(tmp_path).remember("GALE01", "A", 7, "abc")
    assert CardDirectory(tmp_path).state("GALE01", "A").version == 7


def test_unreadable_state_is_treated_as_unknown_not_fatal(tmp_path):
    (tmp_path / ".slotsync.json").write_text("{ not json", encoding="utf-8")
    assert CardDirectory(tmp_path).state("GALE01", "A").version == 0
