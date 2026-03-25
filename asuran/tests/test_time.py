"""Tests for the time domain plugin."""
from __future__ import annotations

import pytest

from asuran.core.types import FaultConfig, Severity
from asuran.domains.time.plugin import (
    NtpFault,
    SkewFault,
    TimeDomain,
    create_domain,
)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestTimeDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "time"

    def test_is_time_domain(self):
        domain = create_domain()
        assert isinstance(domain, TimeDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        names = [ft.name for ft in domain.fault_types()]
        assert "skew" in names
        assert "ntp" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "date" in domain.required_tools()
        assert "systemctl" in domain.required_tools()

    def test_required_capabilities(self):
        domain = create_domain()
        assert "CAP_SYS_TIME" in domain.required_capabilities()


# ---------------------------------------------------------------------------
# SkewFault
# ---------------------------------------------------------------------------


class TestSkewFault:
    def test_properties(self):
        f = SkewFault()
        assert f.name == "skew"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = SkewFault()
        cfg = FaultConfig(params={"offset": 60})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_negative_offset_is_valid(self):
        f = SkewFault()
        cfg = FaultConfig(params={"offset": -300})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_offset_required(self):
        f = SkewFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("offset" in e.lower() for e in errors)

    def test_validate_offset_not_int(self):
        f = SkewFault()
        cfg = FaultConfig(params={"offset": "not-a-number"})
        errors = f.validate(cfg)
        assert any("offset" in e.lower() for e in errors)

    def test_inject_dry_run(self):
        f = SkewFault()
        cfg = FaultConfig(params={"offset": 120}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0

    def test_parameters_list(self):
        f = SkewFault()
        params = f.parameters()
        assert len(params) == 1
        assert params[0].name == "offset"
        assert params[0].type is int
        assert params[0].required is True


# ---------------------------------------------------------------------------
# NtpFault
# ---------------------------------------------------------------------------


class TestNtpFault:
    def test_properties(self):
        f = NtpFault()
        assert f.name == "ntp"
        assert f.severity == Severity.MEDIUM

    def test_validate_always_passes(self):
        f = NtpFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert errors == []

    def test_parameters_empty(self):
        f = NtpFault()
        assert f.parameters() == []

    def test_inject_dry_run(self):
        f = NtpFault()
        cfg = FaultConfig(params={}, dry_run=True)
        fault_id = f.inject(cfg, None)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0
