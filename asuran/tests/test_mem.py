"""Tests for the memory domain plugin."""
from __future__ import annotations

import pytest

from asuran.core.context import ExecutionContext
from asuran.core.types import FaultConfig, Severity
from asuran.domains.memory.plugin import (
    MemoryDomain,
    OomFault,
    PressureFault,
    SwapFault,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestMemoryDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "mem"

    def test_is_memory_domain(self):
        domain = create_domain()
        assert isinstance(domain, MemoryDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "pressure" in names
        assert "oom" in names
        assert "swap" in names

    def test_required_capabilities(self):
        domain = create_domain()
        assert "CAP_SYS_ADMIN" in domain.required_capabilities()


# ---------------------------------------------------------------------------
# PressureFault
# ---------------------------------------------------------------------------


class TestPressureFault:
    def test_properties(self):
        f = PressureFault()
        assert f.name == "pressure"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid_percent(self):
        f = PressureFault()
        cfg = FaultConfig(params={"percent": 50})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_valid_size(self):
        f = PressureFault()
        cfg = FaultConfig(params={"size": "512M"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_neither_specified(self):
        f = PressureFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("percent" in e or "size" in e for e in errors)

    def test_validate_both_specified(self):
        f = PressureFault()
        cfg = FaultConfig(params={"percent": 50, "size": "512M"})
        errors = f.validate(cfg)
        assert any("only one" in e.lower() for e in errors)

    def test_validate_percent_below_range(self):
        f = PressureFault()
        cfg = FaultConfig(params={"percent": 0})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_percent_above_range(self):
        f = PressureFault()
        cfg = FaultConfig(params={"percent": 101})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_invalid_size(self):
        f = PressureFault()
        cfg = FaultConfig(params={"size": "not-a-size"})
        errors = f.validate(cfg)
        assert len(errors) > 0

    def test_inject_dry_run(self, dry_context: ExecutionContext):
        f = PressureFault()
        cfg = FaultConfig(params={"size": "1G"}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        assert dry_context.rollback_manager.active_count() == 1


# ---------------------------------------------------------------------------
# OomFault
# ---------------------------------------------------------------------------


class TestOomFault:
    def test_properties(self):
        f = OomFault()
        assert f.name == "oom"
        assert f.severity == Severity.HIGH

    def test_validate_valid(self):
        f = OomFault()
        cfg = FaultConfig(params={"pid": 1234, "score": 500})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_pid_required(self):
        f = OomFault()
        cfg = FaultConfig(params={"score": 500})
        errors = f.validate(cfg)
        assert any("pid" in e for e in errors)

    def test_validate_score_required(self):
        f = OomFault()
        cfg = FaultConfig(params={"pid": 1234})
        errors = f.validate(cfg)
        assert any("score" in e for e in errors)

    def test_validate_score_out_of_range(self):
        f = OomFault()
        cfg = FaultConfig(params={"pid": 1234, "score": 1001})
        errors = f.validate(cfg)
        assert any("score" in e for e in errors)


# ---------------------------------------------------------------------------
# SwapFault
# ---------------------------------------------------------------------------


class TestSwapFault:
    def test_properties(self):
        f = SwapFault()
        assert f.name == "swap"
        assert f.severity == Severity.LOW

    def test_validate_valid(self):
        f = SwapFault()
        cfg = FaultConfig(params={"swappiness": 60})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_missing(self):
        f = SwapFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("swappiness" in e for e in errors)

    def test_validate_out_of_range(self):
        f = SwapFault()
        cfg = FaultConfig(params={"swappiness": 201})
        errors = f.validate(cfg)
        assert any("swappiness" in e for e in errors)
