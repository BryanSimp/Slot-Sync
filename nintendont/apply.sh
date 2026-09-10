#!/bin/sh
# Graft SlotSync's runtime card sync onto a Nintendont checkout.
#
#   ./apply.sh /path/to/Nintendont
#
# Copies the new kernel sources in, vendors the protocol core out of wii/core
# so there is still only one copy of it in this repo, and applies the hooks
# into Nintendont's own files. Then build Nintendont as you normally would.
#
# Re-runnable: copying is idempotent, and the patch step is skipped if the
# hooks are already in place.

set -e

ND="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

if [ -z "$ND" ]; then
	echo "usage: $0 /path/to/Nintendont" >&2
	exit 2
fi
if [ ! -f "$ND/kernel/GCNCard.c" ]; then
	echo "$ND does not look like a Nintendont checkout (no kernel/GCNCard.c)" >&2
	exit 2
fi

BASE=$(cat "$HERE/patches/BASE_COMMIT" 2>/dev/null || echo "")
if [ -n "$BASE" ] && [ -d "$ND/.git" ]; then
	HAVE=$(cd "$ND" && git rev-parse HEAD 2>/dev/null || echo "")
	if [ "$HAVE" != "$BASE" ]; then
		echo "note: patches were generated against Nintendont $BASE"
		echo "      this checkout is at ${HAVE:-unknown}; if the patch step"
		echo "      fails, the hooks are small enough to apply by hand -- see"
		echo "      patches/0001-slotsync-hooks.patch"
	fi
fi

echo "==> copying the runtime sync engine into $ND/kernel"
cp "$HERE/kernel/SlotSync.c"      "$ND/kernel/"
cp "$HERE/kernel/SlotSync.h"      "$ND/kernel/"
cp "$HERE/kernel/SlotSyncLogic.c" "$ND/kernel/"
cp "$HERE/kernel/SlotSyncLogic.h" "$ND/kernel/"
cp "$HERE/kernel/SlotSyncNet.c"   "$ND/kernel/"
cp "$HERE/kernel/SlotSyncNet.h"   "$ND/kernel/"

echo "==> vendoring the protocol core from wii/core"
mkdir -p "$ND/kernel/slotsync"
for f in protocol.c protocol.h client.c client.h sha256.c sha256.h memcard.c memcard.h; do
	cp "$REPO/wii/core/$f" "$ND/kernel/slotsync/$f"
done

echo "==> applying the hooks"
if grep -q "SlotSync_Init" "$ND/kernel/main.c"; then
	echo "    already applied, skipping"
else
	(cd "$ND" && patch -p1 --batch --binary < "$HERE/patches/0001-slotsync-hooks.patch")
fi

cat <<'EOF'

Done. Build Nintendont as usual (Build.sh, or make in kernel/ then loader/).

On the SD card you need:

  /slotsync/slotsync.cfg   the same file the wrapper reads -- see
                           wii/slotsync.cfg.example
  /slotsync/state.txt      written by the wrapper. Runtime sync refuses to
                           push a card with no entry here, because without one
                           it cannot name a parent version, and a push naming
                           the wrong parent is a silent overwrite.

So: run the wrapper at least once for a game before expecting the kernel to
sync it mid-session.
EOF
