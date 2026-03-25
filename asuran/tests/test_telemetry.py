"""Tests for the telemetry layer: AuditTrail, AsuranDB, MetricsCollector."""
from __future__ import annotations

import os
import time
from pathlib import Path
from unittest.mock import mock_open, patch

import pytest

from asuran.telemetry.audit import AuditEntry, AuditTrail
from asuran.telemetry.database import AsuranDB
from asuran.telemetry.metrics import MetricsCollector


# ---------------------------------------------------------------------------
# AuditTrail
# ---------------------------------------------------------------------------


class TestAuditTrail:
    def test_record_and_count(self):
        trail = AuditTrail()
        assert trail.count() == 0
        trail.record("net delay eth0 --latency 100")
        assert trail.count() == 1

    def test_record_multiple(self):
        trail = AuditTrail()
        trail.record("cmd1", result="ok", fault_id="f1", domain="net")
        trail.record("cmd2", result="fail")
        trail.record("cmd3")
        assert trail.count() == 3

    def test_last_returns_recent(self):
        trail = AuditTrail()
        for i in range(30):
            trail.record(f"cmd-{i}")
        last_20 = trail.last(20)
        assert len(last_20) == 20
        assert last_20[0].command == "cmd-10"
        assert last_20[-1].command == "cmd-29"

    def test_last_fewer_than_n(self):
        trail = AuditTrail()
        trail.record("only-one")
        last_20 = trail.last(20)
        assert len(last_20) == 1
        assert last_20[0].command == "only-one"

    def test_all(self):
        trail = AuditTrail()
        trail.record("a")
        trail.record("b")
        entries = trail.all()
        assert len(entries) == 2
        assert entries[0].command == "a"
        assert entries[1].command == "b"

    def test_entry_fields(self):
        trail = AuditTrail()
        trail.record("test cmd", result="success", fault_id="abc", domain="cpu")
        entry = trail.last(1)[0]
        assert entry.command == "test cmd"
        assert entry.result == "success"
        assert entry.fault_id == "abc"
        assert entry.domain == "cpu"
        assert isinstance(entry.timestamp, float)
        assert entry.timestamp > 0


# ---------------------------------------------------------------------------
# AsuranDB
# ---------------------------------------------------------------------------


class TestAsuranDB:
    @pytest.fixture
    def db(self, tmp_path: Path) -> AsuranDB:
        db_path = str(tmp_path / "test.db")
        db = AsuranDB(db_path=db_path)
        yield db
        db.close()

    def test_create_experiment(self, db: AsuranDB):
        db.create_experiment("exp-1", "Test Experiment", source="test", dry_run=True)
        exp = db.get_experiment("exp-1")
        assert exp is not None
        assert exp["name"] == "Test Experiment"
        assert exp["source"] == "test"
        assert exp["status"] == "running"
        assert exp["dry_run"] == 1

    def test_complete_experiment(self, db: AsuranDB):
        db.create_experiment("exp-2", "Another Exp")
        db.complete_experiment("exp-2", status="completed")
        exp = db.get_experiment("exp-2")
        assert exp["status"] == "completed"
        assert exp["end_time"] is not None

    def test_record_fault(self, db: AsuranDB):
        db.create_experiment("exp-3", "Fault Exp")
        db.record_fault(
            fault_id="f-1",
            experiment_id="exp-3",
            domain="net",
            fault_type="delay",
            severity="LOW",
            config={"latency": 100},
        )
        faults = db.get_faults("exp-3")
        assert len(faults) == 1
        assert faults[0]["id"] == "f-1"
        assert faults[0]["domain"] == "net"
        assert faults[0]["fault_type"] == "delay"
        assert faults[0]["severity"] == "LOW"
        assert faults[0]["status"] == "active"

    def test_complete_fault(self, db: AsuranDB):
        db.create_experiment("exp-4", "Complete Exp")
        db.record_fault("f-2", "exp-4", "cpu", "stress", "MEDIUM", {"cores": 4})
        db.complete_fault("f-2", rollback_status="success")
        faults = db.get_faults("exp-4")
        assert faults[0]["status"] == "completed"
        assert faults[0]["rollback_status"] == "success"

    def test_record_metric(self, db: AsuranDB):
        db.create_experiment("exp-5", "Metric Exp")
        db.record_fault("f-3", "exp-5", "net", "loss", "LOW", {})
        db.record_metric("f-3", "exp-5", "active", "packet_loss", 5.0, "%")
        metrics = db.get_metrics("f-3")
        assert len(metrics) == 1
        assert metrics[0]["metric_name"] == "packet_loss"
        assert metrics[0]["metric_value"] == 5.0
        assert metrics[0]["metric_unit"] == "%"

    def test_record_event(self, db: AsuranDB):
        db.record_event("test_event", "Something happened", level="WARNING", domain="disk")
        events = db.get_events(limit=10)
        assert len(events) == 1
        assert events[0]["event_type"] == "test_event"
        assert events[0]["level"] == "WARNING"
        assert events[0]["domain"] == "disk"

    def test_get_experiments_limit(self, db: AsuranDB):
        for i in range(5):
            db.create_experiment(f"exp-{i}", f"Experiment {i}")
        exps = db.get_experiments(limit=3)
        assert len(exps) == 3

    def test_get_experiment_not_found(self, db: AsuranDB):
        assert db.get_experiment("nonexistent") is None


# ---------------------------------------------------------------------------
# MetricsCollector
# ---------------------------------------------------------------------------


class TestMetricsCollector:
    def test_collect_cpu(self):
        # Mock /proc/stat with known data
        stat_data = "cpu  100 20 30 350 10 5 2 0 0 0\n"
        collector = MetricsCollector()
        with patch("builtins.open", mock_open(read_data=stat_data)):
            result = collector.collect_cpu()
        assert "sys.cpu.percent" in result
        # total = 100+20+30+350+10+5+2 = 517, idle = 350
        # usage = (517-350)/517*100 = 32.30...
        assert 30 < result["sys.cpu.percent"] < 35

    def test_collect_cpu_handles_error(self):
        collector = MetricsCollector()
        with patch("builtins.open", side_effect=FileNotFoundError):
            result = collector.collect_cpu()
        assert result == {}

    def test_collect_memory(self):
        meminfo_data = (
            "MemTotal:       16000000 kB\n"
            "MemFree:         4000000 kB\n"
            "MemAvailable:    8000000 kB\n"
            "SwapTotal:       2000000 kB\n"
            "SwapFree:        1500000 kB\n"
        )
        collector = MetricsCollector()
        with patch("builtins.open", mock_open(read_data=meminfo_data)):
            result = collector.collect_memory()
        assert "sys.mem.total" in result
        assert "sys.mem.available" in result
        assert "sys.mem.percent" in result
        assert "sys.mem.swap_used" in result
        # total=16G bytes, available=8G bytes -> 50% used
        assert result["sys.mem.percent"] == 50.0

    def test_collect_memory_handles_error(self):
        collector = MetricsCollector()
        with patch("builtins.open", side_effect=FileNotFoundError):
            result = collector.collect_memory()
        assert result == {}

    def test_collect_load(self):
        loadavg_data = "1.50 2.30 3.10 1/100 12345\n"
        collector = MetricsCollector()
        with patch("builtins.open", mock_open(read_data=loadavg_data)):
            result = collector.collect_load()
        assert result["sys.cpu.load_1m"] == 1.50
        assert result["sys.cpu.load_5m"] == 2.30
        assert result["sys.cpu.load_15m"] == 3.10

    def test_collect_load_handles_error(self):
        collector = MetricsCollector()
        with patch("builtins.open", side_effect=FileNotFoundError):
            result = collector.collect_load()
        assert result == {}

    def test_collect_all_returns_dict(self):
        """collect_all should return a dict even if /proc files are unavailable."""
        collector = MetricsCollector()
        # On a real Linux system this will have data; otherwise empty dicts are fine
        result = collector.collect_all()
        assert isinstance(result, dict)
