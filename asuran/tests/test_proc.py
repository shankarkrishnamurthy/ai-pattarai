"""Tests for the process domain plugin."""
from __future__ import annotations

import pytest

from asuran.core.context import ExecutionContext
from asuran.core.types import FaultConfig, Severity
from asuran.domains.process.plugin import (
    FreezeFault,
    KillFault,
    ProcessDomain,
    ZombieFault,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestProcessDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "proc"

    def test_is_process_domain(self):
        domain = create_domain()
        assert isinstance(domain, ProcessDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "kill" in names
        assert "freeze" in names
        assert "zombie" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "pgrep" in domain.required_tools()


# ---------------------------------------------------------------------------
# KillFault
# ---------------------------------------------------------------------------


class TestKillFault:
    def test_properties(self):
        f = KillFault()
        assert f.name == "kill"
        assert f.severity == Severity.HIGH

    def test_validate_with_pid(self):
        f = KillFault()
        cfg = FaultConfig(params={"pid": 1234})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_with_name(self):
        f = KillFault()
        cfg = FaultConfig(params={"name": "nginx"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_neither_pid_nor_name(self):
        f = KillFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("pid" in e or "name" in e for e in errors)

    def test_validate_both_pid_and_name(self):
        f = KillFault()
        cfg = FaultConfig(params={"pid": 1234, "name": "nginx"})
        errors = f.validate(cfg)
        assert any("only one" in e.lower() for e in errors)

    def test_validate_invalid_pid(self):
        f = KillFault()
        cfg = FaultConfig(params={"pid": -1})
        errors = f.validate(cfg)
        assert any("pid" in e for e in errors)

    def test_validate_unknown_signal(self):
        f = KillFault()
        cfg = FaultConfig(params={"pid": 1234, "signal": "SIGFAKE"})
        errors = f.validate(cfg)
        assert any("signal" in e.lower() or "Unknown" in e for e in errors)

    def test_validate_valid_signal(self):
        f = KillFault()
        cfg = FaultConfig(params={"pid": 1234, "signal": "SIGKILL"})
        errors = f.validate(cfg)
        assert errors == []

    def test_inject_dry_run(self, dry_context: ExecutionContext):
        f = KillFault()
        cfg = FaultConfig(params={"pid": 99999}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        # Kill is irreversible but rollback is still registered for tracking
        assert dry_context.rollback_manager.active_count() == 1


# ---------------------------------------------------------------------------
# FreezeFault
# ---------------------------------------------------------------------------


class TestFreezeFault:
    def test_properties(self):
        f = FreezeFault()
        assert f.name == "freeze"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = FreezeFault()
        cfg = FaultConfig(params={"pid": 1234})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_pid_required(self):
        f = FreezeFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("pid" in e for e in errors)

    def test_validate_invalid_pid(self):
        f = FreezeFault()
        cfg = FaultConfig(params={"pid": 0})
        errors = f.validate(cfg)
        assert any("pid" in e for e in errors)


# ---------------------------------------------------------------------------
# ZombieFault
# ---------------------------------------------------------------------------


class TestZombieFault:
    def test_properties(self):
        f = ZombieFault()
        assert f.name == "zombie"
        assert f.severity == Severity.LOW

    def test_validate_valid(self):
        f = ZombieFault()
        cfg = FaultConfig(params={"count": 5})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_count_too_high(self):
        f = ZombieFault()
        cfg = FaultConfig(params={"count": 20000})
        errors = f.validate(cfg)
        assert any("count" in e for e in errors)

    def test_validate_count_zero(self):
        f = ZombieFault()
        cfg = FaultConfig(params={"count": 0})
        errors = f.validate(cfg)
        assert any("count" in e for e in errors)

    def test_inject_dry_run(self, dry_context: ExecutionContext):
        f = ZombieFault()
        cfg = FaultConfig(params={"count": 3}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        # No actual child processes should be forked
        assert f._children[fault_id] == []
        assert dry_context.rollback_manager.active_count() == 1
