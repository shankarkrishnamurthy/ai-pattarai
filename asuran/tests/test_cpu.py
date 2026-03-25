"""Tests for the CPU domain plugin."""
from __future__ import annotations

from pathlib import Path

import pytest

from asuran.core.context import ExecutionContext
from asuran.core.types import FaultConfig, Phase, Severity
from asuran.domains.cpu.plugin import (
    CpuDomain,
    StressFault,
    ThrottleFault,
    create_domain,
)
from asuran.safety.rollback import RollbackManager


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestCpuDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "cpu"

    def test_is_cpu_domain(self):
        domain = create_domain()
        assert isinstance(domain, CpuDomain)

    def test_fault_types(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "stress" in names
        assert "throttle" in names

    def test_required_capabilities(self):
        domain = create_domain()
        assert "CAP_SYS_ADMIN" in domain.required_capabilities()


# ---------------------------------------------------------------------------
# StressFault
# ---------------------------------------------------------------------------


class TestStressFault:
    def test_properties(self):
        f = StressFault()
        assert f.name == "stress"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = StressFault()
        cfg = FaultConfig(params={"cores": 2, "load": 80})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_cores_required(self):
        f = StressFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("cores" in e for e in errors)

    def test_validate_cores_zero(self):
        f = StressFault()
        cfg = FaultConfig(params={"cores": 0})
        errors = f.validate(cfg)
        assert any("cores" in e for e in errors)

    def test_validate_cores_negative(self):
        f = StressFault()
        cfg = FaultConfig(params={"cores": -1})
        errors = f.validate(cfg)
        assert any("cores" in e for e in errors)

    def test_validate_load_below_range(self):
        f = StressFault()
        cfg = FaultConfig(params={"cores": 2, "load": 0})
        errors = f.validate(cfg)
        assert any("load" in e for e in errors)

    def test_validate_load_above_range(self):
        f = StressFault()
        cfg = FaultConfig(params={"cores": 2, "load": 101})
        errors = f.validate(cfg)
        assert any("load" in e for e in errors)

    def test_validate_default_load(self):
        f = StressFault()
        cfg = FaultConfig(params={"cores": 4})
        errors = f.validate(cfg)
        assert errors == []  # default load=100 is valid

    def test_inject_dry_run_no_processes(self, dry_context: ExecutionContext):
        """In dry_run mode, no actual worker processes should be spawned."""
        f = StressFault()
        cfg = FaultConfig(params={"cores": 4, "load": 100}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        # Workers list should be empty (no real processes)
        assert f._workers[fault_id] == []
        # Rollback was registered
        assert dry_context.rollback_manager.active_count() == 1


# ---------------------------------------------------------------------------
# ThrottleFault
# ---------------------------------------------------------------------------


class TestThrottleFault:
    def test_properties(self):
        f = ThrottleFault()
        assert f.name == "throttle"
        assert f.severity == Severity.MEDIUM

    def test_validate_percent_required(self):
        f = ThrottleFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_valid(self):
        f = ThrottleFault()
        cfg = FaultConfig(params={"percent": 50})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_percent_below_range(self):
        f = ThrottleFault()
        cfg = FaultConfig(params={"percent": 0})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_percent_above_range(self):
        f = ThrottleFault()
        cfg = FaultConfig(params={"percent": 101})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_invalid_pid(self):
        f = ThrottleFault()
        cfg = FaultConfig(params={"percent": 50, "pid": -1})
        errors = f.validate(cfg)
        assert any("pid" in e for e in errors)

    def test_inject_dry_run(self, dry_context: ExecutionContext):
        f = ThrottleFault()
        cfg = FaultConfig(params={"percent": 50}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        assert fault_id in f._cgroup_paths
        assert dry_context.rollback_manager.active_count() == 1
