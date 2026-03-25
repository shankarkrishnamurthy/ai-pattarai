"""SQLite experiment database."""
from __future__ import annotations

import json
import sqlite3
import time
from pathlib import Path
from typing import Any, Optional


SCHEMA = """
CREATE TABLE IF NOT EXISTS experiments (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    source TEXT NOT NULL DEFAULT 'repl',
    status TEXT NOT NULL DEFAULT 'pending',
    start_time REAL,
    end_time REAL,
    dry_run INTEGER DEFAULT 0,
    tags TEXT,
    hostname TEXT,
    asuran_version TEXT
);

CREATE TABLE IF NOT EXISTS faults (
    id TEXT PRIMARY KEY,
    experiment_id TEXT REFERENCES experiments(id),
    domain TEXT NOT NULL,
    fault_type TEXT NOT NULL,
    severity TEXT,
    config TEXT,
    status TEXT DEFAULT 'pending',
    start_time REAL,
    end_time REAL,
    rollback_status TEXT,
    error_message TEXT
);

CREATE TABLE IF NOT EXISTS metrics (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    fault_id TEXT,
    experiment_id TEXT,
    timestamp REAL,
    phase TEXT,
    metric_name TEXT,
    metric_value REAL,
    metric_unit TEXT
);

CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp REAL,
    level TEXT,
    event_type TEXT,
    experiment_id TEXT,
    fault_id TEXT,
    domain TEXT,
    message TEXT,
    details TEXT
);

CREATE TABLE IF NOT EXISTS baselines (
    id TEXT PRIMARY KEY,
    name TEXT,
    experiment_id TEXT,
    phase TEXT,
    timestamp REAL,
    snapshot TEXT
);
"""


class AsuranDB:
    """SQLite database for experiment tracking."""

    def __init__(self, db_path: Optional[str] = None) -> None:
        path = Path(db_path or "~/.asuran/asuran.db").expanduser()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(str(path), check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)

    def create_experiment(self, exp_id: str, name: str, source: str = "repl",
                          dry_run: bool = False, hostname: str = "", version: str = "") -> None:
        self.conn.execute(
            "INSERT INTO experiments (id, name, source, status, start_time, dry_run, hostname, asuran_version) "
            "VALUES (?, ?, ?, 'running', ?, ?, ?, ?)",
            (exp_id, name, source, time.time(), int(dry_run), hostname, version),
        )
        self.conn.commit()

    def complete_experiment(self, exp_id: str, status: str = "completed") -> None:
        self.conn.execute(
            "UPDATE experiments SET status=?, end_time=? WHERE id=?",
            (status, time.time(), exp_id),
        )
        self.conn.commit()

    def record_fault(self, fault_id: str, experiment_id: str, domain: str,
                     fault_type: str, severity: str, config: dict) -> None:
        self.conn.execute(
            "INSERT INTO faults (id, experiment_id, domain, fault_type, severity, config, status, start_time) "
            "VALUES (?, ?, ?, ?, ?, ?, 'active', ?)",
            (fault_id, experiment_id, domain, fault_type, severity, json.dumps(config), time.time()),
        )
        self.conn.commit()

    def complete_fault(self, fault_id: str, rollback_status: str = "success",
                       error: Optional[str] = None) -> None:
        self.conn.execute(
            "UPDATE faults SET status='completed', end_time=?, rollback_status=?, error_message=? WHERE id=?",
            (time.time(), rollback_status, error, fault_id),
        )
        self.conn.commit()

    def record_metric(self, fault_id: str, experiment_id: str, phase: str,
                      name: str, value: float, unit: str = "") -> None:
        self.conn.execute(
            "INSERT INTO metrics (fault_id, experiment_id, timestamp, phase, metric_name, metric_value, metric_unit) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (fault_id, experiment_id, time.time(), phase, name, value, unit),
        )
        self.conn.commit()

    def record_event(self, event_type: str, message: str, level: str = "INFO",
                     experiment_id: str = "", fault_id: str = "", domain: str = "",
                     details: str = "") -> None:
        self.conn.execute(
            "INSERT INTO events (timestamp, level, event_type, experiment_id, fault_id, domain, message, details) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (time.time(), level, event_type, experiment_id, fault_id, domain, message, details),
        )
        self.conn.commit()

    def get_experiments(self, limit: int = 20) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM experiments ORDER BY start_time DESC LIMIT ?", (limit,)
        ).fetchall()
        return [dict(r) for r in rows]

    def get_experiment(self, exp_id: str) -> Optional[dict]:
        row = self.conn.execute("SELECT * FROM experiments WHERE id=?", (exp_id,)).fetchone()
        return dict(row) if row else None

    def get_faults(self, experiment_id: str) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM faults WHERE experiment_id=? ORDER BY start_time", (experiment_id,)
        ).fetchall()
        return [dict(r) for r in rows]

    def get_metrics(self, fault_id: str) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM metrics WHERE fault_id=? ORDER BY timestamp", (fault_id,)
        ).fetchall()
        return [dict(r) for r in rows]

    def get_events(self, limit: int = 50) -> list[dict]:
        rows = self.conn.execute(
            "SELECT * FROM events ORDER BY timestamp DESC LIMIT ?", (limit,)
        ).fetchall()
        return [dict(r) for r in rows]

    def close(self) -> None:
        self.conn.close()
