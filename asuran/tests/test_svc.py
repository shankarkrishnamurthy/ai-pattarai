"""Tests for the services domain plugin."""
from __future__ import annotations

import pytest

from asuran.core.types import FaultConfig, Severity
from asuran.domains.services.plugin import (
    MaskServiceFault,
    ServicesDomain,
    StopServiceFault,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestServicesDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "svc"

    def test_is_services_domain(self):
        domain = create_domain()
        assert isinstance(domain, ServicesDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "stop" in names
        assert "mask" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "systemctl" in domain.required_tools()

    def test_required_capabilities(self):
        domain = create_domain()
        assert "CAP_SYS_ADMIN" in domain.required_capabilities()


# ---------------------------------------------------------------------------
# StopServiceFault
# ---------------------------------------------------------------------------


class TestStopServiceFault:
    def test_properties(self):
        f = StopServiceFault()
        assert f.name == "stop"
        assert f.severity == Severity.HIGH

    def test_validate_valid(self):
        f = StopServiceFault()
        cfg = FaultConfig(params={"unit": "nginx.service"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_unit_required(self):
        f = StopServiceFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("unit" in e.lower() for e in errors)

    def test_inject_dry_run(self):
        f = StopServiceFault()
        cfg = FaultConfig(params={"unit": "nginx.service"}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = StopServiceFault()
        params = f.parameters()
        assert len(params) == 1
        assert params[0].name == "unit"
        assert params[0].required is True


# ---------------------------------------------------------------------------
# MaskServiceFault
# ---------------------------------------------------------------------------


class TestMaskServiceFault:
    def test_properties(self):
        f = MaskServiceFault()
        assert f.name == "mask"
        assert f.severity == Severity.HIGH

    def test_validate_valid(self):
        f = MaskServiceFault()
        cfg = FaultConfig(params={"unit": "docker.service"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_unit_required(self):
        f = MaskServiceFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("unit" in e.lower() for e in errors)

    def test_inject_dry_run(self):
        f = MaskServiceFault()
        cfg = FaultConfig(params={"unit": "docker.service"}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = MaskServiceFault()
        params = f.parameters()
        assert len(params) == 1
        assert params[0].name == "unit"
        assert params[0].required is True
