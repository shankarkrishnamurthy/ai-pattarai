"""Tests for the kernel domain plugin."""
from __future__ import annotations

import pytest

from asuran.core.types import FaultConfig, Severity
from asuran.domains.kernel.plugin import (
    KernelDomain,
    ModuleFault,
    SysctlFault,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestKernelDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "kern"

    def test_is_kernel_domain(self):
        domain = create_domain()
        assert isinstance(domain, KernelDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "sysctl" in names
        assert "module" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "rmmod" in domain.required_tools()
        assert "modprobe" in domain.required_tools()

    def test_required_capabilities(self):
        domain = create_domain()
        assert "CAP_SYS_ADMIN" in domain.required_capabilities()


# ---------------------------------------------------------------------------
# SysctlFault
# ---------------------------------------------------------------------------


class TestSysctlFault:
    def test_properties(self):
        f = SysctlFault()
        assert f.name == "sysctl"
        assert f.severity == Severity.HIGH

    def test_validate_valid(self):
        f = SysctlFault()
        cfg = FaultConfig(params={"key": "net.ipv4.ip_forward", "value": "0"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_key_required(self):
        f = SysctlFault()
        cfg = FaultConfig(params={"value": "0"})
        errors = f.validate(cfg)
        assert any("key" in e.lower() for e in errors)

    def test_validate_value_required(self):
        f = SysctlFault()
        cfg = FaultConfig(params={"key": "net.ipv4.ip_forward"})
        errors = f.validate(cfg)
        assert any("value" in e.lower() for e in errors)

    def test_validate_both_missing(self):
        f = SysctlFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert len(errors) == 2

    def test_inject_dry_run(self):
        f = SysctlFault()
        cfg = FaultConfig(
            params={"key": "net.ipv4.ip_forward", "value": "0"},
            dry_run=True,
        )
        fault_id = f.inject(cfg, None)  # context not needed for dry_run
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = SysctlFault()
        params = f.parameters()
        names = [p.name for p in params]
        assert "key" in names
        assert "value" in names


# ---------------------------------------------------------------------------
# ModuleFault
# ---------------------------------------------------------------------------


class TestModuleFault:
    def test_properties(self):
        f = ModuleFault()
        assert f.name == "module"
        assert f.severity == Severity.HIGH

    def test_validate_valid(self):
        f = ModuleFault()
        cfg = FaultConfig(params={"name": "dummy"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_name_required(self):
        f = ModuleFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("name" in e.lower() for e in errors)

    def test_inject_dry_run(self):
        f = ModuleFault()
        cfg = FaultConfig(params={"name": "dummy"}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = ModuleFault()
        params = f.parameters()
        names = [p.name for p in params]
        assert "name" in names
