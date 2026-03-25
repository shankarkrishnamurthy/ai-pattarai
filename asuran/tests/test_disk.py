"""Tests for the disk domain plugin."""
from __future__ import annotations

import os

import pytest

from asuran.core.context import ExecutionContext
from asuran.core.types import FaultConfig, Severity
from asuran.domains.disk.plugin import (
    DiskDomain,
    FillFault,
    IoLatencyFault,
    IopsFault,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestDiskDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "disk"

    def test_is_disk_domain(self):
        domain = create_domain()
        assert isinstance(domain, DiskDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "fill" in names
        assert "latency" in names
        assert "iops" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "fallocate" in domain.required_tools()
        assert "dd" in domain.required_tools()


# ---------------------------------------------------------------------------
# FillFault
# ---------------------------------------------------------------------------


class TestFillFault:
    def test_properties(self):
        f = FillFault()
        assert f.name == "fill"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self, tmp_path):
        f = FillFault()
        cfg = FaultConfig(params={"path": str(tmp_path), "size": "1M"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_missing_size(self, tmp_path):
        f = FillFault()
        cfg = FaultConfig(params={"path": str(tmp_path)})
        errors = f.validate(cfg)
        assert any("size" in e for e in errors)

    def test_validate_invalid_size_string(self, tmp_path):
        f = FillFault()
        cfg = FaultConfig(params={"path": str(tmp_path), "size": "abc"})
        errors = f.validate(cfg)
        assert len(errors) > 0

    def test_validate_nonexistent_path(self):
        f = FillFault()
        cfg = FaultConfig(params={"path": "/nonexistent/path/xyz", "size": "1M"})
        errors = f.validate(cfg)
        assert any("path" in e for e in errors)

    def test_validate_empty_path(self):
        f = FillFault()
        cfg = FaultConfig(params={"path": "", "size": "1M"})
        errors = f.validate(cfg)
        assert any("path" in e for e in errors)

    def test_inject_dry_run(self, dry_context: ExecutionContext, tmp_path):
        f = FillFault()
        cfg = FaultConfig(params={"path": str(tmp_path), "size": "10M"}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        # No actual file created
        fill_files = list(tmp_path.glob("asuran_fill_*"))
        assert len(fill_files) == 0
        assert dry_context.rollback_manager.active_count() == 1


# ---------------------------------------------------------------------------
# IoLatencyFault
# ---------------------------------------------------------------------------


class TestIoLatencyFault:
    def test_properties(self):
        f = IoLatencyFault()
        assert f.name == "latency"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = IoLatencyFault()
        cfg = FaultConfig(params={"device": "/dev/sda", "delay": 100})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_missing_device(self):
        f = IoLatencyFault()
        cfg = FaultConfig(params={"delay": 100})
        errors = f.validate(cfg)
        assert any("device" in e for e in errors)

    def test_validate_missing_delay(self):
        f = IoLatencyFault()
        cfg = FaultConfig(params={"device": "/dev/sda"})
        errors = f.validate(cfg)
        assert any("delay" in e for e in errors)

    def test_validate_delay_zero(self):
        f = IoLatencyFault()
        cfg = FaultConfig(params={"device": "/dev/sda", "delay": 0})
        errors = f.validate(cfg)
        assert any("delay" in e for e in errors)


# ---------------------------------------------------------------------------
# IopsFault
# ---------------------------------------------------------------------------


class TestIopsFault:
    def test_properties(self):
        f = IopsFault()
        assert f.name == "iops"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = IopsFault()
        cfg = FaultConfig(params={"device": "/dev/sda", "limit": 100})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_missing_device(self):
        f = IopsFault()
        cfg = FaultConfig(params={"limit": 100})
        errors = f.validate(cfg)
        assert any("device" in e for e in errors)

    def test_validate_missing_limit(self):
        f = IopsFault()
        cfg = FaultConfig(params={"device": "/dev/sda"})
        errors = f.validate(cfg)
        assert any("limit" in e for e in errors)

    def test_validate_limit_zero(self):
        f = IopsFault()
        cfg = FaultConfig(params={"device": "/dev/sda", "limit": 0})
        errors = f.validate(cfg)
        assert any("limit" in e for e in errors)
