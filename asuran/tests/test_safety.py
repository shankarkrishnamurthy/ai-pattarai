"""Tests for the safety layer: RollbackManager, BlastRadiusGuard, SafetyConfig."""
from __future__ import annotations

from pathlib import Path
from typing import Any
from unittest.mock import MagicMock

import pytest

from asuran.core.errors import BlastRadiusError, DurationError, SeverityError
from asuran.core.plugin import FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity
from asuran.safety.limits import BlastRadiusGuard, SafetyConfig
from asuran.safety.rollback import RollbackManager


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


class _FaultWithSeverity(FaultType):
    """A stub FaultType with a configurable severity."""

    def __init__(self, sev: Severity = Severity.LOW):
        self._sev = sev

    @property
    def name(self) -> str:
        return "stub"

    @property
    def description(self) -> str:
        return "stub"

    @property
    def severity(self) -> Severity:
        return self._sev

    def parameters(self) -> list[ParameterSpec]:
        return []

    def validate(self, config: FaultConfig) -> list[str]:
        return []

    def inject(self, config: FaultConfig, context: Any) -> str:
        return "stub-id"

    def rollback(self, fault_id: str, context: Any) -> None:
        pass


# ---------------------------------------------------------------------------
# RollbackManager
# ---------------------------------------------------------------------------


class TestRollbackManager:
    def test_push_increments_count(self, rollback_mgr: RollbackManager):
        assert rollback_mgr.active_count() == 0
        rollback_mgr.push("f1", lambda: None, "desc1")
        assert rollback_mgr.active_count() == 1
        rollback_mgr.push("f2", lambda: None, "desc2")
        assert rollback_mgr.active_count() == 2

    def test_rollback_one(self, rollback_mgr: RollbackManager):
        called = []
        rollback_mgr.push("f1", lambda: called.append("f1"), "desc1")
        result = rollback_mgr.rollback_one("f1")
        assert result is True
        assert called == ["f1"]
        assert rollback_mgr.active_count() == 0

    def test_rollback_one_unknown_id(self, rollback_mgr: RollbackManager):
        result = rollback_mgr.rollback_one("nonexistent")
        assert result is False

    def test_rollback_all(self, rollback_mgr: RollbackManager):
        called = []
        rollback_mgr.push("f1", lambda: called.append("f1"), "desc1")
        rollback_mgr.push("f2", lambda: called.append("f2"), "desc2")
        rollback_mgr.push("f3", lambda: called.append("f3"), "desc3")
        count = rollback_mgr.rollback_all()
        assert count == 3
        assert rollback_mgr.active_count() == 0

    def test_rollback_all_lifo_order(self, rollback_mgr: RollbackManager):
        order = []
        rollback_mgr.push("f1", lambda: order.append("f1"), "first")
        rollback_mgr.push("f2", lambda: order.append("f2"), "second")
        rollback_mgr.push("f3", lambda: order.append("f3"), "third")
        rollback_mgr.rollback_all()
        assert order == ["f3", "f2", "f1"], "Rollback must be LIFO (last-in, first-out)"

    def test_rollback_idempotent(self, rollback_mgr: RollbackManager):
        call_count = 0

        def action():
            nonlocal call_count
            call_count += 1

        rollback_mgr.push("f1", action, "desc1")
        rollback_mgr.rollback_one("f1")
        # Second call should be idempotent (already rolled back)
        result = rollback_mgr.rollback_one("f1")
        assert result is True  # returns True because fault_id is in _rolled_back
        assert call_count == 1  # action was only called once

    def test_active_faults_list(self, rollback_mgr: RollbackManager):
        rollback_mgr.push("f1", lambda: None, "desc1", domain="net", fault_type="delay")
        faults = rollback_mgr.active_faults()
        assert len(faults) == 1
        assert faults[0]["fault_id"] == "f1"
        assert faults[0]["domain"] == "net"
        assert faults[0]["fault_type"] == "delay"

    def test_state_file_persists(self, tmp_path: Path):
        state_dir = tmp_path / ".asuran"
        rm = RollbackManager(state_dir=state_dir)
        rm.push("f1", lambda: None, "desc1", domain="cpu", fault_type="stress")
        state_file = state_dir / "active_faults.json"
        assert state_file.exists()

    def test_clear_state(self, tmp_path: Path):
        state_dir = tmp_path / ".asuran"
        rm = RollbackManager(state_dir=state_dir)
        rm.push("f1", lambda: None, "desc1")
        state_file = state_dir / "active_faults.json"
        assert state_file.exists()
        rm.clear_state()
        assert not state_file.exists()

    def test_recover_from_crash_empty(self, tmp_path: Path):
        state_dir = tmp_path / ".asuran"
        rm = RollbackManager(state_dir=state_dir)
        # No previous state
        data = rm.recover_from_crash()
        assert data == []

    def test_recover_from_crash_with_data(self, tmp_path: Path):
        state_dir = tmp_path / ".asuran"
        rm1 = RollbackManager(state_dir=state_dir)
        rm1.push("f1", lambda: None, "test desc", domain="net", fault_type="delay")
        # Simulate a new manager reading old state
        rm2 = RollbackManager(state_dir=state_dir)
        data = rm2.recover_from_crash()
        assert len(data) == 1
        assert data[0]["fault_id"] == "f1"


# ---------------------------------------------------------------------------
# BlastRadiusGuard
# ---------------------------------------------------------------------------


class TestBlastRadiusGuard:
    def test_check_passes_within_limits(self):
        guard = BlastRadiusGuard(SafetyConfig(max_concurrent_faults=5))
        fault = _FaultWithSeverity(Severity.LOW)
        config = FaultConfig(params={}, dry_run=True)
        # Should not raise
        guard.check(fault, config, active_count=2)

    def test_check_fails_max_concurrent(self):
        guard = BlastRadiusGuard(SafetyConfig(max_concurrent_faults=3))
        fault = _FaultWithSeverity(Severity.LOW)
        config = FaultConfig(params={}, dry_run=True)
        with pytest.raises(BlastRadiusError, match="Max concurrent"):
            guard.check(fault, config, active_count=3)

    def test_check_fails_severity_too_high(self):
        guard = BlastRadiusGuard(SafetyConfig(max_severity=Severity.MEDIUM))
        fault = _FaultWithSeverity(Severity.CRITICAL)
        config = FaultConfig(params={}, dry_run=True)
        with pytest.raises(SeverityError, match="Severity"):
            guard.check(fault, config, active_count=0)

    def test_check_severity_at_limit_passes(self):
        guard = BlastRadiusGuard(SafetyConfig(max_severity=Severity.HIGH))
        fault = _FaultWithSeverity(Severity.HIGH)
        config = FaultConfig(params={}, dry_run=True)
        # Should not raise: severity == max_severity is allowed
        guard.check(fault, config, active_count=0)

    def test_check_fails_duration_too_long(self):
        guard = BlastRadiusGuard(SafetyConfig(max_absolute_duration=60.0))
        fault = _FaultWithSeverity(Severity.LOW)
        config = FaultConfig(params={}, duration=120.0, dry_run=True)
        with pytest.raises(DurationError, match="Duration"):
            guard.check(fault, config, active_count=0)

    def test_check_applies_default_duration(self):
        guard = BlastRadiusGuard(SafetyConfig(max_default_duration=45.0))
        fault = _FaultWithSeverity(Severity.LOW)
        config = FaultConfig(params={}, dry_run=False)
        assert config.duration is None
        guard.check(fault, config, active_count=0)
        assert config.duration == 45.0

    def test_is_protected_interface(self):
        guard = BlastRadiusGuard(SafetyConfig(protected_interfaces=["lo", "eth0"]))
        assert guard.is_protected_interface("lo") is True
        assert guard.is_protected_interface("eth1") is False

    def test_is_protected_process(self):
        guard = BlastRadiusGuard()
        assert guard.is_protected_process("sshd") is True
        assert guard.is_protected_process("myapp") is False

    def test_is_protected_path(self):
        guard = BlastRadiusGuard()
        assert guard.is_protected_path("/boot") is True
        assert guard.is_protected_path("/home") is False

    def test_is_domain_disabled(self):
        guard = BlastRadiusGuard(SafetyConfig(disabled_domains=["kern"]))
        assert guard.is_domain_disabled("kern") is True
        assert guard.is_domain_disabled("net") is False

    def test_needs_confirmation(self):
        guard = BlastRadiusGuard(SafetyConfig(confirm_above_severity=Severity.MEDIUM))
        low = _FaultWithSeverity(Severity.LOW)
        med = _FaultWithSeverity(Severity.MEDIUM)
        high = _FaultWithSeverity(Severity.HIGH)
        assert guard.needs_confirmation(low) is False
        assert guard.needs_confirmation(med) is True
        assert guard.needs_confirmation(high) is True


# ---------------------------------------------------------------------------
# SafetyConfig defaults
# ---------------------------------------------------------------------------


class TestSafetyConfig:
    def test_defaults(self):
        cfg = SafetyConfig()
        assert cfg.max_default_duration == 300.0
        assert cfg.max_absolute_duration == 3600.0
        assert cfg.max_concurrent_faults == 10
        assert cfg.max_severity == Severity.HIGH
        assert cfg.confirm_above_severity == Severity.MEDIUM
        assert cfg.disabled_domains == []
        assert "lo" in cfg.protected_interfaces
        assert "sshd" in cfg.protected_processes
        assert "/boot" in cfg.protected_paths
        assert cfg.dry_run_default is False
