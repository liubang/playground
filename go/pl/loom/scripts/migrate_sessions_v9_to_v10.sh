#!/bin/sh
# migrate_sessions_v9_to_v10.sh — one-off schema migration for the loom
# session store (SQLite), schema v9 -> v10.
#
# Background: loom has no in-code migrations by design (dev-local databases
# only; internal/session/sqlite_store.go fails loudly on version mismatch —
# its own comment names this script shape as the sanctioned fix: "recreate
# the dev database or migrate it with a one-off script").
#
# v10 changed exactly one thing: file_changes gained a run_id column
# (per-turn file-revert attribution). The migration is safe in place:
#   1. ALTER TABLE file_changes ADD COLUMN run_id TEXT NOT NULL DEFAULT ''
#      Rows recorded before the upgrade stay run-less ('') and are
#      ineligible for per-turn revert — the correct conservative answer (no
#      turn identity is recoverable for them; checkpoint rewind is
#      position-based and unaffected).
#   2. INSERT the schema_migrations row for v10 so OpenSQLiteStore opens
#      again (ReadOnly and read-write entry points alike).
#
# Usage:
#   migrate_sessions_v9_to_v10.sh [path-to-sessions.db]
# Default: ${LOOM_HOME:-$HOME/.loom}/sessions/sessions.db
#
# Guarantees:
#   - aborts unless the database is EXACTLY at schema version 9 (idempotent:
#     rerunning against an already-migrated v10 database is a no-op exit 0);
#   - refuses to double-apply (run_id present but version is 9 = inconsistent
#     state, left untouched);
#   - backs up with an online, WAL-consistent copy (VACUUM INTO) before
#     writing — plain `cp` of a WAL database can silently lose un-checkpointed
#     pages;
#   - applies the two writes in one transaction.
#
# Quit the desktop app / `loom serve` first: WAL lets a single other writer
# block the ALTER and this script would fail on SQLITE_BUSY.

set -eu

DB="${1:-${LOOM_HOME:-$HOME/.loom}/sessions/sessions.db}"
SQLITE3="$(command -v sqlite3 || true)"
if [ -z "$SQLITE3" ]; then
    echo "error: sqlite3 not found in PATH" >&2
    exit 1
fi
if [ ! -f "$DB" ]; then
    echo "error: database not found: $DB" >&2
    echo "pass the path explicitly, or set LOOM_HOME" >&2
    exit 1
fi

VERSION="$($SQLITE3 "$DB" "SELECT MAX(version) FROM schema_migrations;" 2>/dev/null || true)"
case "$VERSION" in
"10")
    echo "already at v10, nothing to do: $DB"
    exit 0
    ;;
"9") ;;
*)
    echo "error: schema_migrations MAX(version) = '${VERSION:-<none>}' (expected 9), refusing to touch" >&2
    exit 1
    ;;
esac
if $SQLITE3 "$DB" "PRAGMA table_info(file_changes);" | grep -q '|run_id|'; then
    echo "error: file_changes already has run_id but version is 9 — inconsistent state; leaving untouched" >&2
    exit 1
fi

BACKUP="$DB.v9-backup"
if [ -e "$BACKUP" ]; then
    echo "error: backup already exists: $BACKUP" >&2
    echo "remove it first (or restore from it) to rerun" >&2
    exit 1
fi
echo "backing up (online, WAL-safe): $BACKUP"
$SQLITE3 "$DB" "VACUUM INTO '$BACKUP';"

$SQLITE3 "$DB" <<'SQL'
BEGIN IMMEDIATE;
ALTER TABLE file_changes ADD COLUMN run_id TEXT NOT NULL DEFAULT '';
-- Same shape as store formatTime (RFC3339 UTC); strftime %f gives
-- SS.SSS, so the fractional part stays inside the seconds field.
INSERT INTO schema_migrations(version, applied_at)
VALUES (10, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));
COMMIT;
SQL

NEW_VERSION="$($SQLITE3 "$DB" "SELECT MAX(version) FROM schema_migrations;")"
if [ "$NEW_VERSION" != "10" ]; then
    echo "error: migration did not stick (version = '$NEW_VERSION'); restore with:" >&2
    echo "  mv '$BACKUP' '$DB'" >&2
    exit 1
fi
ROWS="$($SQLITE3 "$DB" "SELECT COUNT(*) FROM file_changes;")"
echo "migrated: $DB (v9 -> v10), file_changes rows preserved: $ROWS"
echo "verify: sqlite3 '$DB' 'PRAGMA table_info(file_changes);'   # expect a run_id row"
