"""Command line for the Dolphin daemon.

slotsync-dolphin setup --server http://nas:8080 --token ...
slotsync-dolphin status
slotsync-dolphin play GALE01 --exec ~/games/zelda.iso
slotsync-dolphin watch
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from .client import Client, Conflict, ServerError
from .config import Config, ConfigError
from .dolphin import default_user_dir, find_executable, is_running
from .sync import Syncer, SyncError

log = logging.getLogger("slotsync")


def _configure_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(message)s",
        stream=sys.stderr,
    )


# --- commands -------------------------------------------------------------


def cmd_setup(config: Config, args) -> int:
    """Write the config file so later commands need no flags."""
    client = Client(config.server, config.token)
    reachable = client.healthy()

    path = config.save()
    print(f"wrote {path}")
    print(
        f"  server    {config.server}  ({'reachable' if reachable else 'NOT reachable'})"
    )
    print(f"  cards     {config.cards_dir}")
    print(f"  slot      {config.slot}")

    user_dir = config.user_dir or default_user_dir()
    print(f"  dolphin   {user_dir or 'NOT FOUND -- pass --user-dir'}")
    exe = config.dolphin_exe or find_executable()
    print(f"  exe       {exe or 'NOT FOUND -- pass --dolphin-exe for `play`'}")

    if not reachable:
        print("\nThe server did not answer. Check --server and --token.", file=sys.stderr)
        return 1
    return 0


def cmd_status(config: Config, args) -> int:
    """What is on disk, what is on the server, and where they differ."""
    syncer = Syncer(config)
    cards = syncer.cards

    print(f"server   {config.server}")
    print(f"cards    {cards.root}")

    try:
        dolphin = syncer.dolphin_config()
        index = 1 if config.slot.upper() == "B" else 0
        print(f"dolphin  {dolphin.paths.config}")
        print(
            f"  slot {config.slot}: {dolphin.describe(index)} -> "
            f"{dolphin.memcard_path(index)}"
        )
    except SyncError as exc:
        print(f"dolphin  {exc}")

    if is_running():
        print(
            "  NOTE: Dolphin is running; card changes cannot be applied until it exits."
        )

    remote = {}
    try:
        for card in syncer.client.list_cards():
            remote[(card["game_id"], card["slot_name"])] = card
    except ServerError as exc:
        print(f"\nserver unreachable: {exc}", file=sys.stderr)

    local = cards.known_games()
    keys = sorted(set(local) | set(remote.keys()))
    if not keys:
        print("\nNothing here yet. Try: slotsync-dolphin play GALE01")
        return 0

    print(f"\n{'GAME':<8} {'SLOT':<5} {'LOCAL':<10} {'SERVER':<8} STATE")
    for game_id, slot in keys:
        state = cards.state(game_id, slot)
        has_local = cards.exists(game_id, slot)
        head = remote.get((game_id, slot), {}).get("head")

        if not has_local:
            situation = "server only"
        elif cards.has_local_changes(game_id, slot):
            situation = "LOCAL CHANGES -- push"
        elif head is not None and head > state.version:
            situation = "server ahead -- pull"
        else:
            situation = "in sync"

        print(
            f"{game_id:<8} {slot:<5} "
            f"{('v' + str(state.version)) if has_local else '-':<10} "
            f"{('v' + str(head)) if head else '-':<8} {situation}"
        )
    return 0


def cmd_pull(config: Config, args) -> int:
    outcome = Syncer(config).pull(args.game_id, args.slot)
    print(f"{outcome.game_id} slot {outcome.slot}: {outcome.action} v{outcome.version}")
    print(f"  {outcome.path}")
    return 0


def cmd_push(config: Config, args) -> int:
    outcome = Syncer(config).push(args.game_id, args.slot)
    if outcome.action == "nothing":
        print(f"{outcome.game_id} slot {outcome.slot}: no local changes")
    else:
        print(
            f"{outcome.game_id} slot {outcome.slot}: {outcome.action} "
            f"as v{outcome.version}"
        )
    return 0


def cmd_use(config: Config, args) -> int:
    """Pull and point Dolphin at a game, without launching it."""
    syncer = Syncer(config)
    outcome = syncer.pull(args.game_id, args.slot)
    changed = syncer.point_dolphin_at(
        args.game_id, args.slot or config.slot, force=args.force
    )

    print(f"{outcome.game_id} slot {outcome.slot}: {outcome.action} v{outcome.version}")
    print(f"  Dolphin.ini {'updated' if changed else 'already pointed there'}")
    print(f"  card {outcome.path}")
    print("\nStart Dolphin and play. Run `push` when you are done.")
    return 0


def cmd_play(config: Config, args) -> int:
    syncer = Syncer(config)
    pulled, pushed = syncer.play(
        args.game_id,
        Path(args.exec) if args.exec else None,
        args.slot,
        launch=not args.no_launch,
        force=args.force,
    )
    print(f"{pulled.game_id} slot {pulled.slot}: {pulled.action} v{pulled.version}")
    if pushed is None:
        print("  Dolphin not launched; run `push` when you are done.")
    elif pushed.action == "nothing":
        print("  no changes to push")
    else:
        print(f"  {pushed.action} as v{pushed.version}")
    return 0


def cmd_watch(config: Config, args) -> int:
    try:
        Syncer(config).watch()
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


def cmd_list(config: Config, args) -> int:
    """Saves on a card, as the server parses them."""
    detail = Syncer(config).client.card(args.game_id, args.slot or config.slot)
    if detail is None:
        print(f"the server has no card for {args.game_id}")
        return 1

    card = detail["card"]
    print(f"{detail['game_id']} slot {detail['slot_name']}  v{detail['head']['version']}")
    print(
        f"  {card['size_bytes'] // (1024 * 1024)} MiB, "
        f"{card['used_blocks']}/{card['data_blocks']} blocks used"
    )
    for warning in card["warnings"]:
        print(f"  WARNING: {warning}")
    for save in detail["saves"]:
        print(
            f"  {save['game_code']}{save['maker_code']}  {save['blocks']:>4} blk  "
            f"{save['display_name']}"
        )
    return 0


# --- entry point ----------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="slotsync-dolphin",
        description="Sync Dolphin's GameCube memory cards with a SlotSync server.",
    )
    parser.add_argument("--server", help="e.g. http://nas:8080")
    parser.add_argument("--token", help="defaults to $SLOTSYNC_TOKEN")
    parser.add_argument("--cards-dir", help="where per-game cards are kept")
    parser.add_argument("--user-dir", help="Dolphin's user directory")
    parser.add_argument("--dolphin-exe", help="path to the Dolphin binary")
    parser.add_argument("--slot", choices=["A", "B", "a", "b"], help="default A")
    parser.add_argument("--device", type=lambda v: int(v, 0), help="u64 device id")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "--force",
        action="store_true",
        help="edit Dolphin.ini even though Dolphin is running (it may undo the change)",
    )

    sub = parser.add_subparsers(dest="command", required=True)

    setup = sub.add_parser("setup", help="write the config file")
    setup.add_argument("--mbit", type=int, help="card size for new games, default 16")
    setup.set_defaults(func=cmd_setup)

    sub.add_parser("status", help="what is local, what is on the server").set_defaults(
        func=cmd_status
    )

    for name, func, helptext in [
        ("pull", cmd_pull, "fetch a game's card from the server"),
        ("push", cmd_push, "send a game's card to the server"),
        ("use", cmd_use, "pull and point Dolphin at a game, without launching"),
        ("list", cmd_list, "show the saves on a game's card"),
    ]:
        one = sub.add_parser(name, help=helptext)
        one.add_argument("game_id")
        one.add_argument("--slot", choices=["A", "B", "a", "b"])
        one.set_defaults(func=func)

    play = sub.add_parser("play", help="pull, launch Dolphin, push on exit")
    play.add_argument("game_id")
    play.add_argument("--slot", choices=["A", "B", "a", "b"])
    play.add_argument("--exec", help="ISO to boot directly")
    play.add_argument(
        "--no-launch",
        action="store_true",
        help="set the card up but do not start Dolphin",
    )
    play.set_defaults(func=cmd_play)

    watch = sub.add_parser("watch", help="push cards as they change")
    watch.set_defaults(func=cmd_watch)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    _configure_logging(args.verbose)

    try:
        config = Config.load(
            server=args.server,
            token=args.token,
            cards_dir=args.cards_dir,
            user_dir=args.user_dir,
            dolphin_exe=args.dolphin_exe,
            slot=args.slot,
            device=args.device,
            mbit=getattr(args, "mbit", None),
        )
        return args.func(config, args)

    except Conflict as exc:
        print(
            f"\nCONFLICT: another device pushed version {exc.head} while you were "
            f"playing.\n"
            f"Nothing has been overwritten and your card is still on disk.\n"
            f"Open the web UI to compare the two and choose which to keep.",
            file=sys.stderr,
        )
        return 3
    except (ConfigError, SyncError, ServerError) as exc:
        print(f"slotsync-dolphin: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
