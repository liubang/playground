#!/usr/bin/env python3
"""Offline, explicitly invoked migration. Requires the repository's Go schema source."""

import argparse
import fcntl
import hashlib
import json
import os
import re
import shutil
import sqlite3
import stat
import sys
import tempfile
from contextlib import closing, contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

SOURCE = Path(__file__).absolute().parent.parent / "internal/session/sqlite_store.go"
NOTES_DIR = Path("extensions/ad_hoc/notes")
NEW_TABLES = {"memory_scope_state", "memory_records", "memory_views", "memory_requests"}
RECORD_COLUMNS = (
    "id",
    "scope",
    "kind",
    "content",
    "rollout",
    "revision",
    "sequence",
    "source_session_id",
    "source_version",
    "updated_at",
)
# Same ordered finite detector set as internal/memory/redact.go. ASCII word
# boundaries match Go/RE2, including when a token abuts non-ASCII prose.
SECRET_PATTERNS = (
    (
        r"(?s)-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----.*?-----END [A-Z0-9 ]*PRIVATE KEY-----",
        "[REDACTED_PRIVATE_KEY]",
    ),
    (r"\bsk-[A-Za-z0-9_-]{16,}\b", "[REDACTED_SECRET]"),
    (r"\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}\b", "[REDACTED_SECRET]"),
    (r"\bgithub_pat_[A-Za-z0-9_]{20,}\b", "[REDACTED_SECRET]"),
    (r"\bAKIA[0-9A-Z]{16}\b", "[REDACTED_SECRET]"),
    (r"\bxox[baprs]-[A-Za-z0-9-]{10,}\b", "[REDACTED_SECRET]"),
    (
        r"\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b",
        "[REDACTED_JWT]",
    ),
    (r"(?i)(bearer\s+)[A-Za-z0-9._~+/=-]{16,}", r"\g<1>[REDACTED_SECRET]"),
    (
        r"""(?i)\b(api[_-]?key|api[_-]?secret|secret[_-]?key|access[_-]?token|auth[_-]?token|refresh[_-]?token|private[_-]?token|password|passwd)(["']?\s*[:=]\s*["']?)[A-Za-z0-9._~+/=!@#$%^*-]{8,}(["']?)""",
        r"\g<1>\g<2>[REDACTED_SECRET]\g<3>",
    ),
)


class MigrationError(Exception):
    """A safe diagnostic that never embeds note content."""


def utc_now():
    return (
        datetime.now(timezone.utc)
        .isoformat(timespec="microseconds")
        .replace("+00:00", "Z")
    )


def statements(ddl):
    pending = ""
    for char in ddl:
        pending += char
        if char == ";" and sqlite3.complete_statement(pending):
            yield pending
            pending = ""
    if re.sub(r"--[^\n]*", "", pending).strip():
        raise MigrationError("incomplete repository schema")


def load_schema(source=SOURCE):
    text = source.read_text(encoding="utf-8")
    versions = re.findall(r"\bconst\s+sqliteSchemaVersion\s*=\s*(\d+)", text)
    schemas = re.findall(r"\bconst\s+schema\s*=\s*`([^`]*)`", text, re.DOTALL)
    if versions != ["12"] or len(schemas) != 1:
        raise MigrationError("repository must declare exactly one v12 schema")
    ddl = tuple(statements(schemas[0]))
    with closing(sqlite3.connect(":memory:")) as reference:
        for statement in ddl:
            reference.execute(statement)
        tables = table_layout(reference)
    return ddl, tables


def table_layout(db):
    names = db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"
    ).fetchall()
    return {
        name: {row[1]: row[2:] for row in db.execute(f'PRAGMA table_info("{name}")')}
        for (name,) in names
    }


def version(db):
    value = db.execute("SELECT MAX(version) FROM schema_migrations").fetchone()[0]
    if value not in (9, 10, 11, 12):
        raise MigrationError(
            "only existing schema v9/v10/v11/v12 databases are supported"
        )
    return value


def validate_schema(db, expected):
    current_version = version(db)
    actual = table_layout(db)
    optional = NEW_TABLES if current_version < 12 else set()
    if not set(expected) - optional <= set(actual):
        raise MigrationError("required legacy tables are missing")
    allowed = (
        {
            ("memory_jobs", "attempt_version"),
            ("memory_jobs", "claimed_evidence_cutoff_unix_nano"),
        }
        if current_version < 12
        else set()
    )
    if current_version == 9:
        allowed.add(("file_changes", "run_id"))
    if current_version <= 10:
        allowed.add(("sessions", "title"))
    missing = []
    for table, columns in expected.items():
        if table not in actual:
            continue
        if set(actual[table]) - set(columns):
            raise MigrationError("unexpected legacy columns")
        for name, definition in columns.items():
            if name not in actual[table]:
                if (table, name) not in allowed:
                    raise MigrationError("unexpected missing legacy column")
                missing.append((table, name))
            elif actual[table][name] != definition:
                raise MigrationError(
                    "legacy column definition differs from repository schema"
                )
    return current_version, missing


def redact(text):
    for pattern, replacement in SECRET_PATTERNS:
        text = re.sub(pattern, replacement, text, flags=re.ASCII)
    return text


@dataclass(frozen=True)
class Note:
    request_id: str
    payload_hash: str
    content: str


@contextmanager
def tree_files(root):
    """Yield regular files via anchored descriptors; refuse links/special files.

    Keeping parent descriptors open and O_NOFOLLOW prevents directory swaps
    from redirecting note reads or backup copies outside the legacy tree.
    """

    def walk(fd, relative):
        with os.scandir(fd) as entries:
            for entry in sorted(entries, key=lambda item: item.name):
                info = entry.stat(follow_symlinks=False)
                path = relative / entry.name
                if stat.S_ISDIR(info.st_mode):
                    child = os.open(
                        entry.name,
                        os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                        dir_fd=fd,
                    )
                    try:
                        yield from walk(child, path)
                    finally:
                        os.close(child)
                elif stat.S_ISREG(info.st_mode):
                    child = os.open(
                        entry.name,
                        os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                        dir_fd=fd,
                    )
                    try:
                        if not stat.S_ISREG(os.fstat(child).st_mode):
                            raise MigrationError("legacy tree contains a special file")
                        yield path, child
                    finally:
                        os.close(child)
                else:
                    raise MigrationError(
                        "legacy tree contains a symlink or special file"
                    )

    try:
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except FileNotFoundError:
        yield iter(())
        return
    try:
        yield walk(fd, Path())
    finally:
        os.close(fd)


def preflight_notes(root):
    notes = []
    with tree_files(root) as files:
        for relative, fd in files:
            if NOTES_DIR not in relative.parents or relative.suffix != ".md":
                continue
            with os.fdopen(os.dup(fd), "rb") as stream:
                raw = stream.read()
            try:
                original = raw.decode("utf-8").strip()
            except UnicodeDecodeError:
                raise MigrationError("note is not valid UTF-8") from None
            content = redact(original).strip()
            if (
                not original
                or len(original.encode("utf-8")) > 4096
                or not content
                or len(content.encode("utf-8")) > 4096
            ):
                raise MigrationError(
                    "note must contain 1..4096 UTF-8 bytes after trimming"
                )
            request = "legacy-note:" + relative.as_posix()
            # Hash original bytes as well, so editing a secret/whitespace cannot
            # silently reuse a receipt even when sanitized content is identical.
            payload = json.dumps(
                ["legacy-note-v12", request, hashlib.sha256(raw).hexdigest()],
                separators=(",", ":"),
                ensure_ascii=True,
            )
            notes.append(
                Note(request, hashlib.sha256(payload.encode()).hexdigest(), content)
            )
    return notes


def pending_notes(db, notes):
    if "memory_requests" not in table_layout(db):
        return notes
    pending = []
    for note in notes:
        receipt = db.execute(
            "SELECT payload_hash FROM memory_requests WHERE request_id=?",
            (note.request_id,),
        ).fetchone()
        if receipt is None:
            pending.append(note)
        elif receipt[0] != note.payload_hash:
            raise MigrationError("legacy note request payload mismatch")
    return pending


def check_integrity(db):
    if db.execute("PRAGMA integrity_check").fetchall() != [("ok",)]:
        raise MigrationError("database integrity check failed")
    if db.execute("PRAGMA foreign_key_check").fetchone() is not None:
        raise MigrationError("database foreign key check failed")


def protected_counts(db):
    return {
        name: db.execute(f'SELECT COUNT(*) FROM "{name}"').fetchone()[0]
        for name in table_layout(db)
        if (not name.startswith("memory_") and name != "schema_migrations")
        or name == "memory_jobs"
    }


def create_backup(db, home, legacy):
    parent = home / "backups"
    if parent.is_symlink():
        raise MigrationError("backup parent must not be a symlink")
    parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    target = Path(tempfile.mkdtemp(prefix=f"memory-v12-{stamp}-", dir=parent))
    try:
        path = target / "sessions.db"
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        with closing(sqlite3.connect(path)) as destination:
            db.backup(destination)
            check_integrity(destination)
        with tree_files(legacy) as files:
            for relative, source_fd in files:
                output = target / "memories" / relative
                output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
                with os.fdopen(os.dup(source_fd), "rb") as source:
                    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                    with os.fdopen(fd, "wb") as destination:
                        shutil.copyfileobj(source, destination)
        # Preserve empty directories as well, with no traversal through links.
        if legacy.exists():
            for root, dirs, _ in os.walk(legacy, followlinks=False):
                for name in dirs:
                    path = Path(root) / name
                    if path.is_symlink():
                        raise MigrationError("legacy tree changed during backup")
                    (target / "memories" / path.relative_to(legacy)).mkdir(
                        mode=0o700, parents=True, exist_ok=True
                    )
            (target / "memories").mkdir(mode=0o700, exist_ok=True)
        return target
    except BaseException:
        shutil.rmtree(target)
        raise


def add_columns(db, ddl, missing):
    # Derive ALTER column definitions from the same CREATE source, never a
    # second hand-maintained schema. These four columns have one-line DDL.
    for table, column in missing:
        create = next(
            s for s in ddl if re.search(rf"CREATE TABLE IF NOT EXISTS {table}\s*\(", s)
        )
        definition = re.search(rf"^\s*({column}\s+[^\n]+)", create, re.MULTILINE)
        if definition is None:
            raise MigrationError("cannot derive migration column from schema")
        db.execute(
            f'ALTER TABLE "{table}" ADD COLUMN {definition[1].strip().rstrip(",")}'
        )


def import_note(db, note):
    row = db.execute(
        "SELECT "
        + ",".join(RECORD_COLUMNS)
        + " FROM memory_records WHERE scope='' AND kind='explicit' AND content=? ORDER BY sequence DESC, id LIMIT 1",
        (note.content,),
    ).fetchone()
    created = row is None
    if created:
        sequence = db.execute(
            "INSERT INTO memory_scope_state(scope,version,epoch) VALUES ('',1,1) ON CONFLICT(scope) DO UPDATE SET version=version+1,epoch=epoch+1 RETURNING version"
        ).fetchone()[0]
        db.execute("DELETE FROM memory_views WHERE scope=''")
        record_id = "mem_" + hashlib.sha256(note.request_id.encode()).hexdigest()[:32]
        row = (
            record_id,
            "",
            "explicit",
            note.content,
            "",
            1,
            sequence,
            "",
            0,
            utc_now(),
        )
        db.execute(
            "INSERT INTO memory_records ("
            + ",".join(RECORD_COLUMNS)
            + ") VALUES (?,?,?,?,?,?,?,?,?,?)",
            row,
        )
    response = dict(zip(RECORD_COLUMNS, row))
    response.pop("source_session_id")
    # Match Go's omitzero SessionID representation for global records.
    db.execute(
        "INSERT INTO memory_requests(request_id,payload_hash,record_id,response) VALUES (?,?,?,?)",
        (
            note.request_id,
            note.payload_hash,
            row[0],
            json.dumps(response, ensure_ascii=False).encode(),
        ),
    )
    return int(created)


@contextmanager
def exclusive_lock(db_path):
    fd = os.open(
        db_path.parent / "loom.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600
    )
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise MigrationError(
                "loom.lock is held; stop Loom before migrating"
            ) from None
        yield
    finally:
        os.close(fd)


def connect(path, mode):
    db = sqlite3.connect(
        path.as_uri() + "?mode=" + mode, uri=True, isolation_level=None, timeout=5
    )
    db.execute("PRAGMA foreign_keys=ON")
    return db


def migrate(home, db_path=None, legacy=None, dry_run=False):
    ddl, expected = load_schema()
    home = Path(home).expanduser().absolute()
    db_path = (
        Path(db_path).expanduser().absolute()
        if db_path
        else home / "sessions/sessions.db"
    )
    legacy = Path(legacy).expanduser().absolute() if legacy else home / "memories"
    if not db_path.is_file() or db_path.is_symlink():
        raise MigrationError("an existing regular database is required")
    with exclusive_lock(db_path), closing(connect(db_path, "ro")) as reader:
        reader.execute("BEGIN")
        original_version, _ = validate_schema(reader, expected)
        check_integrity(reader)
        notes = preflight_notes(legacy)
        pending = pending_notes(reader, notes)
        if dry_run or (original_version == 12 and not pending):
            return {
                "version_before": original_version,
                "version": 12,
                "notes": len(notes),
                "requests": len(pending),
                "records": 0,
                "backup": None,
                "dry_run": dry_run,
            }
        backup = create_backup(reader, home, legacy)
        reader.execute("ROLLBACK")
        if notes != preflight_notes(legacy):
            raise MigrationError("legacy notes changed during backup")
        with closing(connect(db_path, "rw")) as writer:
            writer.execute("BEGIN IMMEDIATE")
            try:
                current_version, missing = validate_schema(writer, expected)
                if current_version != original_version:
                    raise MigrationError("schema version changed during backup")
                before = protected_counts(writer)
                pending = pending_notes(writer, notes)
                if current_version < 12:
                    add_columns(writer, ddl, missing)
                    for statement in ddl:
                        writer.execute(statement)
                    writer.execute(
                        "UPDATE memory_jobs SET extracted_version=-1,attempt_version=-1,attempts=0,status='pending',claim_token='',claimed_at_unix_nano=0,claimed_evidence_cutoff_unix_nano=0,next_retry_at_unix_nano=0,last_error=''"
                    )
                    writer.execute(
                        "INSERT INTO schema_migrations(version,applied_at) VALUES (12,?)",
                        (utc_now(),),
                    )
                created = sum(import_note(writer, note) for note in pending)
                validate_schema(writer, expected)
                check_integrity(writer)
                if protected_counts(writer) != before:
                    raise MigrationError("protected table counts changed")
                writer.execute("COMMIT")
            except BaseException:
                writer.execute("ROLLBACK")
                raise
        return {
            "version_before": original_version,
            "version": 12,
            "notes": len(notes),
            "requests": len(pending),
            "records": created,
            "backup": str(backup),
            "dry_run": False,
        }


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Offline memory v12 migration; stop Loom first. Repository source is required."
    )
    parser.add_argument("--home", default=os.environ.get("LOOM_HOME") or "~/.loom")
    parser.add_argument("--db")
    parser.add_argument("--legacy-dir")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = migrate(args.home, args.db, args.legacy_dir, args.dry_run)
    except MigrationError as error:
        print(f"migration refused: {error}", file=sys.stderr)
        return 1
    except (OSError, sqlite3.Error, ValueError):
        # sqlite errors and filesystem names can contain attacker-controlled
        # text; do not print exception details or tracebacks containing secrets.
        print(
            "migration failed: filesystem or SQLite operation failed; database transaction was not committed",
            file=sys.stderr,
        )
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
