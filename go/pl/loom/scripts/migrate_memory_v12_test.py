"""Regression tests: all database and legacy files live in temporary homes."""

import fcntl
import hashlib
import io
import json
import os
import re
import sqlite3
import stat
import tempfile
import unittest
from contextlib import closing, redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

import migrate_memory_v12 as migration


class MigrationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.path = self.home / "sessions/sessions.db"
        self.path.parent.mkdir()
        self.legacy = self.home / "memories"
        self.ddl, self.layout = migration.load_schema()
        self.make_database(11)

    def make_database(self, version):
        if self.path.exists():
            self.path.unlink()
        with closing(sqlite3.connect(self.path)) as db:
            for statement in self.ddl:
                if version < 12 and any(
                    re.search(rf"\b{name}\b", statement)
                    for name in migration.NEW_TABLES
                ):
                    continue
                if version < 12:
                    statement = re.sub(
                        r"^\s*(attempt_version|claimed_evidence_cutoff_unix_nano)\s+[^\n]+\n",
                        "",
                        statement,
                        flags=re.MULTILINE,
                    )
                if version <= 10:
                    statement = re.sub(
                        r"^\s*title\s+[^\n]+\n", "", statement, flags=re.MULTILINE
                    )
                if version <= 9:
                    statement = re.sub(
                        r"^\s*run_id\s+[^\n]+\n", "", statement, flags=re.MULTILINE
                    )
                statement = re.sub(r",\s*\)", "\n)", statement)
                db.execute(statement)
            db.execute(
                "INSERT INTO schema_migrations VALUES (?, '2026-01-01T00:00:00Z')",
                (version,),
            )
            self.insert_row(db, "sessions", session_id="sess_test", version=1)
            self.insert_row(
                db,
                "events",
                event_id="evt_test",
                session_id="sess_test",
                sequence=1,
                type="user",
                payload=b'{"message":"keep"}',
            )
            self.insert_row(
                db,
                "checkpoints",
                checkpoint_id="ckpt_test",
                session_id="sess_test",
                sequence=1,
                data=b"checkpoint",
            )
            self.insert_row(
                db,
                "artifact_refs",
                session_id="sess_test",
                artifact_id="artifact_test",
                size=9,
            )
            self.insert_row(
                db,
                "file_changes",
                session_id="sess_test",
                path="a.go",
                before_content=b"before",
                before_existed=1,
            )
            self.insert_row(
                db, "session_shares", session_id="sess_test", share_token="share_test"
            )
            self.insert_row(
                db,
                "workspaces",
                workspace_id="ws_test",
                name="workspace",
                root_path="/workspace",
            )
            self.insert_row(db, "app_prefs", pref_key="model", pref_value="keep")
            self.insert_row(
                db,
                "memory_jobs",
                session_id="sess_test",
                workspace_root="/original/root",
                status="running",
                attempts=4,
                claim_token="old_claim",
                claimed_at_unix_nano=77,
                extracted_version=13,
                next_retry_at_unix_nano=88,
                last_error="old_error",
            )
            db.commit()

    @staticmethod
    def insert_row(db, table, **values):
        for _, name, kind, required, default, _ in db.execute(
            f"PRAGMA table_info({table})"
        ):
            if name not in values and required and default is None:
                values[name] = (
                    0
                    if kind == "INTEGER"
                    else (b"" if kind == "BLOB" else "2026-01-01T00:00:00Z")
                )
        db.execute(
            f"INSERT INTO {table} ({','.join(values)}) VALUES ({','.join('?' for _ in values)})",
            tuple(values.values()),
        )

    def note(self, name="one.md", content="prefer explicit tests"):
        path = self.legacy / migration.NOTES_DIR / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def run_migration(self, **kwargs):
        return migration.migrate(self.home, **kwargs)

    def query(self, sql, args=()):
        with closing(sqlite3.connect(self.path)) as db:
            return db.execute(sql, args).fetchall()

    def snapshot(self):
        with closing(sqlite3.connect(self.path)) as db:
            return list(db.iterdump())

    def assert_preflight_refused(self, error=migration.MigrationError):
        before = self.snapshot()
        with self.assertRaises(error):
            self.run_migration()
        self.assertEqual(before, self.snapshot())
        self.assertFalse((self.home / "backups").exists())

    def test_upgrade_versions_and_preserve_core(self):
        for old_version in (9, 10, 11):
            with self.subTest(version=old_version):
                self.make_database(old_version)
                path = self.note()
                original = path.read_bytes()
                with closing(sqlite3.connect(self.path)) as db:
                    core = {
                        table: (
                            list(columns),
                            db.execute(
                                f"SELECT {','.join(columns)} FROM {table}"
                            ).fetchall(),
                        )
                        for table, columns in migration.table_layout(db).items()
                        if not table.startswith("memory_")
                        and table != "schema_migrations"
                    }
                result = self.run_migration()
                self.assertEqual(
                    (old_version, 12, 1, 1),
                    (
                        result["version_before"],
                        result["version"],
                        result["requests"],
                        result["records"],
                    ),
                )
                with closing(sqlite3.connect(self.path)) as db:
                    self.assertEqual(self.layout, migration.table_layout(db))
                    for table, (columns, rows) in core.items():
                        self.assertEqual(
                            rows,
                            db.execute(
                                f"SELECT {','.join(columns)} FROM {table}"
                            ).fetchall(),
                            table,
                        )
                self.assertEqual(
                    [(12,)], self.query("SELECT MAX(version) FROM schema_migrations")
                )
                self.assertEqual(
                    [("/original/root", "pending", 0, -1, -1, "", 0, 0, 0, "")],
                    self.query(
                        "SELECT workspace_root,status,attempts,attempt_version,extracted_version,claim_token,claimed_at_unix_nano,claimed_evidence_cutoff_unix_nano,next_retry_at_unix_nano,last_error FROM memory_jobs"
                    ),
                )
                request = "legacy-note:" + path.relative_to(self.legacy).as_posix()
                record_id = "mem_" + hashlib.sha256(request.encode()).hexdigest()[:32]
                self.assertEqual(
                    [(record_id, "", "explicit", original.decode(), 1, 1)],
                    self.query(
                        "SELECT id,scope,kind,content,revision,sequence FROM memory_records"
                    ),
                )
                receipt = json.loads(
                    self.query("SELECT response FROM memory_requests")[0][0]
                )
                self.assertEqual(record_id, receipt["id"])
                backup = Path(result["backup"])
                self.assertEqual(0o700, stat.S_IMODE(backup.stat().st_mode))
                self.assertEqual(
                    0o600, stat.S_IMODE((backup / "sessions.db").stat().st_mode)
                )
                self.assertEqual(
                    original,
                    (backup / "memories" / path.relative_to(self.legacy)).read_bytes(),
                )
                self.assertEqual(original, path.read_bytes())
                with closing(sqlite3.connect(backup / "sessions.db")) as db:
                    self.assertEqual(old_version, migration.version(db))

    def test_no_notes_and_no_summary_promotion(self):
        self.legacy.mkdir()
        for name in (
            "MEMORY.md",
            "memory_summary.md",
            "raw_memories.md",
            "rollout_summaries/session.md",
            ".git/config",
        ):
            path = self.legacy / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("automatic knowledge", encoding="utf-8")
        (self.legacy / "empty").mkdir()
        result = self.run_migration()
        self.assertEqual(0, result["notes"])
        self.assertEqual([], self.query("SELECT * FROM memory_records"))
        self.assertTrue((Path(result["backup"]) / "memories/empty").is_dir())
        for source in self.legacy.rglob("*"):
            if source.is_file():
                self.assertEqual(
                    source.read_bytes(),
                    (
                        Path(result["backup"])
                        / "memories"
                        / source.relative_to(self.legacy)
                    ).read_bytes(),
                )

    def test_absent_legacy(self):
        self.assertEqual(0, self.run_migration()["notes"])

    def test_replay_forget_no_resurrection_and_new_notes(self):
        path = self.note()
        self.run_migration()
        before = self.snapshot()
        self.assertIsNone(self.run_migration()["backup"])
        self.assertEqual(before, self.snapshot())
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("DELETE FROM memory_records")
            db.execute(
                "UPDATE memory_requests SET response=json_set(CAST(response AS TEXT),'$.content','')"
            )
            db.execute(
                "UPDATE memory_jobs SET status='done',attempts=8,extracted_version=42"
            )
            db.commit()
        before = self.snapshot()
        self.assertEqual(0, self.run_migration()["requests"])
        self.assertEqual(before, self.snapshot())
        self.assertEqual([], self.query("SELECT * FROM memory_records"))
        self.note("new.md", "new explicit fact")
        result = self.run_migration()
        self.assertEqual((12, 1), (result["version_before"], result["records"]))
        self.assertEqual(
            [("new explicit fact",)], self.query("SELECT content FROM memory_records")
        )
        self.assertEqual(
            [("done", 8, 42)],
            self.query("SELECT status,attempts,extracted_version FROM memory_jobs"),
        )
        path.write_text("modified same path", encoding="utf-8")
        before = self.snapshot()
        backups = list((self.home / "backups").iterdir())
        with self.assertRaisesRegex(migration.MigrationError, "payload mismatch"):
            self.run_migration()
        self.assertEqual(before, self.snapshot())
        self.assertEqual(backups, list((self.home / "backups").iterdir()))

    def test_exact_content_dedup_and_scope_no_advance(self):
        self.note("a.md", "  same content\n")
        self.note("b.md", "same content")
        self.note("nested/c.md", "same content")
        result = self.run_migration()
        self.assertEqual((3, 1), (result["requests"], result["records"]))
        self.assertEqual(
            [(1, 1)],
            self.query("SELECT version,epoch FROM memory_scope_state WHERE scope=''"),
        )
        self.assertEqual(
            [(1,)], self.query("SELECT COUNT(DISTINCT record_id) FROM memory_requests")
        )
        before = self.query("SELECT * FROM memory_scope_state")
        self.note("d.md", "same content")
        self.assertEqual(0, self.run_migration()["records"])
        self.assertEqual(before, self.query("SELECT * FROM memory_scope_state"))

    def test_all_redaction_patterns_and_no_content_in_output(self):
        secrets = [
            "-----BEGIN RSA PRIVATE KEY-----\nprivate material\n-----END RSA PRIVATE KEY-----",
            "sk-" + "a" * 20,
            "sk-ant-" + "b" * 20,
            "sk-proj-" + "c" * 20,
            *(
                prefix + "a" * 20
                for prefix in ("ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_")
            ),
            "AKIA" + "A" * 16,
            *("xox" + prefix + "-" + "a" * 12 for prefix in "baprs"),
            "eyJ" + "a" * 8 + "." + "b" * 8 + "." + "c" * 8,
            "Bearer " + "z" * 20,
            *(
                key + '="' + "a" * 12 + '"'
                for key in (
                    "api_key",
                    "api-key",
                    "apikey",
                    "API_SECRET",
                    "secret_key",
                    "access-token",
                    "auth_token",
                    "refresh_token",
                    "private_token",
                    "password",
                    "passwd",
                )
            ),
        ]
        self.note(content="\n".join(secrets) + "\n中文sk-" + "q" * 20 + "中文")
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(0, migration.main(["--home", str(self.home)]))
        content = self.query("SELECT content FROM memory_records")[0][0]
        response = self.query("SELECT response FROM memory_requests")[0][0].decode()
        for secret in secrets:
            self.assertNotIn(secret, content)
            self.assertNotIn(secret, response)
            self.assertNotIn(secret, stdout.getvalue() + stderr.getvalue())
        self.assertIn("[REDACTED_PRIVATE_KEY]", content)
        self.assertIn("[REDACTED_JWT]", content)
        self.assertIn('password="[REDACTED_SECRET]"', content)
        self.assertIn("中文[REDACTED_SECRET]中文", content)
        self.assertEqual(
            "short sk-abc; password=short; useful examples",
            migration.redact("short sk-abc; password=short; useful examples"),
        )

    def test_invalid_notes_preflight_is_atomic(self):
        for content in ("好" * 1366, "a" * 4097, " \n\t", "\x80"):
            with self.subTest(content_length=len(content)):
                self.note("a-valid.md", "valid note")
                path = self.note("z-invalid.md", content)
                if content == "\x80":
                    path.write_bytes(b"\x80")
                self.assert_preflight_refused()
                path.unlink()
        self.note("boundary.md", "好" * 1365 + "a")
        self.assertEqual(2, self.run_migration()["records"])

    def test_reject_bad_versions_missing_and_malformed_schema(self):
        for value in (8, 13, None):
            self.make_database(11)
            with closing(sqlite3.connect(self.path)) as db:
                db.execute("DELETE FROM schema_migrations")
                if value is not None:
                    db.execute("INSERT INTO schema_migrations VALUES (?, '')", (value,))
                db.commit()
            self.assert_preflight_refused()
        self.make_database(11)
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("ALTER TABLE sessions DROP COLUMN title")
        self.assert_preflight_refused()
        self.path.unlink()
        with self.assertRaises(migration.MigrationError):
            self.run_migration()
        self.assertFalse(self.path.exists())

    def test_repository_version_is_checked_before_any_write(self):
        source = self.home / "schema.go"
        source.write_text(
            migration.SOURCE.read_text().replace(
                "sqliteSchemaVersion = 12", "sqliteSchemaVersion = 13"
            ),
            encoding="utf-8",
        )
        original = migration.load_schema
        with patch.object(
            migration, "load_schema", side_effect=lambda: original(source)
        ):
            self.assert_preflight_refused()
        self.assertEqual(
            ["SELECT ';';", " SELECT 1;"],
            list(migration.statements("SELECT ';'; SELECT 1;")),
        )

    def test_flock_refuses_even_dryrun(self):
        with open(self.path.parent / "loom.lock", "wb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assert_preflight_refused()
            with self.assertRaisesRegex(migration.MigrationError, "loom.lock"):
                self.run_migration(dry_run=True)

    def test_dryrun_and_cli_defaults_are_readonly(self):
        self.note()
        before = self.snapshot()
        stdout = io.StringIO()
        with (
            patch.dict(os.environ, {"LOOM_HOME": str(self.home)}),
            redirect_stdout(stdout),
        ):
            self.assertEqual(0, migration.main(["--dry-run"]))
        result = json.loads(stdout.getvalue())
        self.assertEqual(
            (11, 1, True, None),
            (
                result["version_before"],
                result["notes"],
                result["dry_run"],
                result["backup"],
            ),
        )
        self.assertEqual(before, self.snapshot())
        self.assertFalse((self.home / "backups").exists())
        self.assertEqual(
            1, self.run_migration(db_path=self.path, legacy=self.legacy)["records"]
        )

    def test_symlink_file_directory_and_root_refused(self):
        outside = self.home / "outside"
        outside.mkdir()
        (outside / "note.md").write_text("outside", encoding="utf-8")
        note = self.note()
        for location, target in (
            (note, outside / "note.md"),
            (note.parent / "escape", outside),
        ):
            if location.exists():
                location.unlink()
            location.symlink_to(target)
            self.assert_preflight_refused()
            location.unlink()
        moved = self.home / "old-memories"
        self.legacy.rename(moved)
        self.legacy.symlink_to(moved, target_is_directory=True)
        self.assert_preflight_refused(OSError)

    def test_backup_contains_uncheckpointed_wal(self):
        with closing(sqlite3.connect(self.path)) as keeper:
            keeper.execute("PRAGMA journal_mode=WAL")
            keeper.execute("PRAGMA wal_autocheckpoint=0")
            keeper.execute("UPDATE app_prefs SET pref_value='only in WAL'")
            keeper.commit()
            self.assertGreater(Path(str(self.path) + "-wal").stat().st_size, 0)
            result = self.run_migration()
            with closing(
                sqlite3.connect(Path(result["backup"]) / "sessions.db")
            ) as backup:
                self.assertEqual(
                    [("only in WAL",)],
                    backup.execute("SELECT pref_value FROM app_prefs").fetchall(),
                )
                self.assertEqual(11, migration.version(backup))

    def test_backup_failure_and_transaction_failure_rollback(self):
        self.note()
        with patch.object(
            migration.shutil, "copyfileobj", side_effect=OSError("injected")
        ):
            before = self.snapshot()
            with self.assertRaises(OSError):
                self.run_migration()
            self.assertEqual(before, self.snapshot())
            self.assertEqual([], list((self.home / "backups").iterdir()))
        before = self.snapshot()
        with (
            patch.object(
                migration,
                "import_note",
                side_effect=sqlite3.OperationalError("injected"),
            ),
            self.assertRaises(sqlite3.Error),
        ):
            self.run_migration()
        self.assertEqual(before, self.snapshot())
        with closing(sqlite3.connect(self.path)) as db:
            self.assertNotIn("memory_records", migration.table_layout(db))
            self.assertNotIn(
                "attempt_version", migration.table_layout(db)["memory_jobs"]
            )

    def test_counts_check_rolls_back_trigger_damage(self):
        self.note()
        with closing(sqlite3.connect(self.path)) as db:
            db.execute(
                "CREATE TRIGGER damage AFTER UPDATE ON memory_jobs BEGIN DELETE FROM events; END"
            )
        before = self.snapshot()
        with self.assertRaisesRegex(migration.MigrationError, "counts changed"):
            self.run_migration()
        self.assertEqual(before, self.snapshot())

    def test_changed_secret_same_redacted_payload_is_refused(self):
        path = self.note(content="sk-" + "a" * 20)
        self.run_migration()
        path.write_text("sk-" + "b" * 20, encoding="utf-8")
        before = self.snapshot()
        with self.assertRaisesRegex(migration.MigrationError, "payload mismatch"):
            self.run_migration()
        self.assertEqual(before, self.snapshot())

    def test_foreign_key_failure_is_preflight(self):
        with closing(sqlite3.connect(self.path)) as db:
            db.execute("UPDATE events SET session_id='missing'")
            db.commit()
        self.assert_preflight_refused()

    def test_integrity_failure_before_commit_rolls_back(self):
        self.note()
        before = self.snapshot()
        original = migration.check_integrity

        def reject_v12(db):
            original(db)
            if migration.version(db) == 12:
                raise migration.MigrationError("injected integrity failure")

        with (
            patch.object(migration, "check_integrity", side_effect=reject_v12),
            self.assertRaises(migration.MigrationError),
        ):
            self.run_migration()
        self.assertEqual(before, self.snapshot())

    def test_version_rechecked_inside_transaction(self):
        real_backup = migration.create_backup

        def racing_backup(*args):
            target = real_backup(*args)
            # Simulates an uncooperative writer ignoring loom.lock (in WAL).
            with closing(sqlite3.connect(self.path)) as writer:
                writer.execute("UPDATE schema_migrations SET version=10")
                writer.commit()
            return target

        with closing(sqlite3.connect(self.path)) as keeper:
            keeper.execute("PRAGMA journal_mode=WAL")
            with (
                patch.object(migration, "create_backup", side_effect=racing_backup),
                self.assertRaisesRegex(migration.MigrationError, "version changed"),
            ):
                self.run_migration()
        self.assertEqual([(10,)], self.query("SELECT version FROM schema_migrations"))
        with closing(sqlite3.connect(self.path)) as db:
            self.assertNotIn("memory_records", migration.table_layout(db))


if __name__ == "__main__":
    unittest.main()
