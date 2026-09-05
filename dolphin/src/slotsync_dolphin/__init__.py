"""SlotSync Dolphin daemon.

Keeps a directory of per-game GameCube memory cards and syncs them with a
SlotSync server, repointing Dolphin's `MemcardAPath` so the PC uses the same
one-card-per-game unit the console does. See PLAN.md section 5.

Stdlib only, deliberately: this runs on a gaming PC where "a Python script with
no dependencies" is much easier to run than one needing a virtualenv first.
"""

__version__ = "0.1.0"
