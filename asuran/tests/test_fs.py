"""Tests for the filesystem domain plugin."""
from __future__ import annotations

import pytest

from asuran.core.types import FaultConfig, Severity
from asuran.domains.filesystem.plugin import (
    FilesystemDomain,
    InodesFault,
    PermissionsFault,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestFilesystemDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "fs"

    def test_is_filesystem_domain(self):
        domain = create_domain()
        assert isinstance(domain, FilesystemDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "permissions" in names
        assert "inodes" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "chmod" in domain.required_tools()
        assert "stat" in domain.required_tools()

    def test_required_capabilities(self):
        domain = create_domain()
        assert "CAP_DAC_OVERRIDE" in domain.required_capabilities()


# ---------------------------------------------------------------------------
# PermissionsFault
# ---------------------------------------------------------------------------


class TestPermissionsFault:
    def test_properties(self):
        f = PermissionsFault()
        assert f.name == "permissions"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = PermissionsFault()
        cfg = FaultConfig(params={"path": "/tmp/somefile", "mode": "000"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_path_required(self):
        f = PermissionsFault()
        cfg = FaultConfig(params={"mode": "000"})
        errors = f.validate(cfg)
        assert any("path" in e.lower() for e in errors)

    def test_validate_mode_required(self):
        f = PermissionsFault()
        cfg = FaultConfig(params={"path": "/tmp/somefile"})
        errors = f.validate(cfg)
        assert any("mode" in e.lower() for e in errors)

    def test_validate_both_missing(self):
        f = PermissionsFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert len(errors) == 2

    def test_inject_dry_run(self):
        f = PermissionsFault()
        cfg = FaultConfig(params={"path": "/tmp/test", "mode": "000"}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = PermissionsFault()
        params = f.parameters()
        names = [p.name for p in params]
        assert "path" in names
        assert "mode" in names


# ---------------------------------------------------------------------------
# InodesFault
# ---------------------------------------------------------------------------


class TestInodesFault:
    def test_properties(self):
        f = InodesFault()
        assert f.name == "inodes"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = InodesFault()
        cfg = FaultConfig(params={"path": "/tmp"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_path_required(self):
        f = InodesFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("path" in e.lower() for e in errors)

    def test_inject_dry_run(self):
        f = InodesFault()
        cfg = FaultConfig(params={"path": "/tmp", "count": 100}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = InodesFault()
        params = f.parameters()
        names = [p.name for p in params]
        assert "path" in names
        assert "count" in names
        count_spec = [p for p in params if p.name == "count"][0]
        assert count_spec.default == 10000
