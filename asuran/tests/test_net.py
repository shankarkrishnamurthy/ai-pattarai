"""Tests for the network domain plugin."""
from __future__ import annotations

from pathlib import Path

import pytest

from asuran.core.context import ExecutionContext
from asuran.core.types import FaultConfig, Phase, Severity
from asuran.domains.network.plugin import (
    BlackholeFault,
    BandwidthFault,
    CorruptFault,
    DelayFault,
    DnsFailFault,
    LossFault,
    NetworkDomain,
    create_domain,
)
from asuran.safety.rollback import RollbackManager


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class TestNetworkDomain:
    def test_name(self):
        domain = create_domain()
        assert domain.name == "net"

    def test_is_network_domain(self):
        domain = create_domain()
        assert isinstance(domain, NetworkDomain)

    def test_fault_types_list(self):
        domain = create_domain()
        fts = domain.fault_types()
        names = [ft.name for ft in fts]
        assert "delay" in names
        assert "loss" in names
        assert "corrupt" in names
        assert "bandwidth" in names
        assert "blackhole" in names
        assert "dns-fail" in names

    def test_required_tools(self):
        domain = create_domain()
        assert "tc" in domain.required_tools()
        assert "iptables" in domain.required_tools()


# ---------------------------------------------------------------------------
# DelayFault
# ---------------------------------------------------------------------------


class TestDelayFault:
    def test_properties(self):
        f = DelayFault()
        assert f.name == "delay"
        assert f.severity == Severity.LOW
        assert len(f.parameters()) > 0

    def test_validate_valid(self):
        f = DelayFault()
        cfg = FaultConfig(params={"interface": "eth0", "latency": 100, "jitter": 10})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_missing_interface(self):
        f = DelayFault()
        cfg = FaultConfig(params={"latency": 100})
        errors = f.validate(cfg)
        assert any("interface" in e for e in errors)

    def test_validate_missing_latency(self):
        f = DelayFault()
        cfg = FaultConfig(params={"interface": "eth0"})
        errors = f.validate(cfg)
        assert any("latency" in e for e in errors)

    def test_validate_negative_latency(self):
        f = DelayFault()
        cfg = FaultConfig(params={"interface": "eth0", "latency": -10})
        errors = f.validate(cfg)
        assert any("latency" in e for e in errors)

    def test_validate_negative_jitter(self):
        f = DelayFault()
        cfg = FaultConfig(params={"interface": "eth0", "latency": 100, "jitter": -5})
        errors = f.validate(cfg)
        assert any("jitter" in e for e in errors)

    def test_inject_dry_run(self, dry_context: ExecutionContext):
        f = DelayFault()
        cfg = FaultConfig(
            params={"interface": "eth0", "latency": 100, "jitter": 10},
            dry_run=True,
        )
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        assert len(fault_id) > 0
        # Verify rollback was registered
        assert dry_context.rollback_manager.active_count() == 1


# ---------------------------------------------------------------------------
# LossFault
# ---------------------------------------------------------------------------


class TestLossFault:
    def test_validate_valid(self):
        f = LossFault()
        cfg = FaultConfig(params={"interface": "eth0", "percent": 50.0})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_percent_too_high(self):
        f = LossFault()
        cfg = FaultConfig(params={"interface": "eth0", "percent": 101.0})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_percent_negative(self):
        f = LossFault()
        cfg = FaultConfig(params={"interface": "eth0", "percent": -1.0})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_percent_missing(self):
        f = LossFault()
        cfg = FaultConfig(params={"interface": "eth0"})
        errors = f.validate(cfg)
        assert any("percent" in e for e in errors)

    def test_validate_zero_percent(self):
        f = LossFault()
        cfg = FaultConfig(params={"interface": "eth0", "percent": 0})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_100_percent(self):
        f = LossFault()
        cfg = FaultConfig(params={"interface": "eth0", "percent": 100})
        errors = f.validate(cfg)
        assert errors == []

    def test_severity(self):
        f = LossFault()
        assert f.severity == Severity.MEDIUM


# ---------------------------------------------------------------------------
# BlackholeFault
# ---------------------------------------------------------------------------


class TestBlackholeFault:
    def test_validate_valid(self):
        f = BlackholeFault()
        cfg = FaultConfig(params={"dst": "10.0.0.1"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_dst_required(self):
        f = BlackholeFault()
        cfg = FaultConfig(params={})
        errors = f.validate(cfg)
        assert any("dst" in e for e in errors)

    def test_validate_empty_dst(self):
        f = BlackholeFault()
        cfg = FaultConfig(params={"dst": ""})
        errors = f.validate(cfg)
        assert any("dst" in e for e in errors)

    def test_validate_with_port(self):
        f = BlackholeFault()
        cfg = FaultConfig(params={"dst": "10.0.0.1", "port": 443, "protocol": "tcp"})
        errors = f.validate(cfg)
        assert errors == []

    def test_validate_invalid_port(self):
        f = BlackholeFault()
        cfg = FaultConfig(params={"dst": "10.0.0.1", "port": 99999})
        errors = f.validate(cfg)
        assert any("port" in e for e in errors)

    def test_validate_invalid_protocol(self):
        f = BlackholeFault()
        cfg = FaultConfig(params={"dst": "10.0.0.1", "protocol": "icmp"})
        errors = f.validate(cfg)
        assert any("protocol" in e for e in errors)

    def test_severity(self):
        f = BlackholeFault()
        assert f.severity == Severity.HIGH

    def test_inject_dry_run(self, dry_context: ExecutionContext):
        f = BlackholeFault()
        cfg = FaultConfig(params={"dst": "10.0.0.1"}, dry_run=True)
        fault_id = f.inject(cfg, dry_context)
        assert isinstance(fault_id, str)
        assert dry_context.rollback_manager.active_count() == 1


# ---------------------------------------------------------------------------
# CorruptFault / BandwidthFault / DnsFailFault — basic checks
# ---------------------------------------------------------------------------


class TestCorruptFault:
    def test_name_and_severity(self):
        f = CorruptFault()
        assert f.name == "corrupt"
        assert f.severity == Severity.MEDIUM

    def test_validate_valid(self):
        f = CorruptFault()
        cfg = FaultConfig(params={"interface": "eth0", "percent": 10})
        assert f.validate(cfg) == []


class TestBandwidthFault:
    def test_name(self):
        f = BandwidthFault()
        assert f.name == "bandwidth"

    def test_validate_valid(self):
        f = BandwidthFault()
        cfg = FaultConfig(params={"interface": "eth0", "rate": "1mbit"})
        assert f.validate(cfg) == []

    def test_validate_missing_rate(self):
        f = BandwidthFault()
        cfg = FaultConfig(params={"interface": "eth0"})
        errors = f.validate(cfg)
        assert any("rate" in e for e in errors)


class TestDnsFailFault:
    def test_name(self):
        f = DnsFailFault()
        assert f.name == "dns-fail"

    def test_validate_always_passes(self):
        f = DnsFailFault()
        cfg = FaultConfig(params={})
        assert f.validate(cfg) == []

    def test_severity(self):
        f = DnsFailFault()
        assert f.severity == Severity.HIGH
