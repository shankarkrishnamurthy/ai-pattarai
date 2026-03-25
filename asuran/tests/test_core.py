"""Tests for the core framework: types, plugin, registry, context, executor."""
from __future__ import annotations

import subprocess
import uuid
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock

import pytest

from asuran.core.context import ExecutionContext
from asuran.core.errors import AsuranError, PluginError, ValidationError
from asuran.core.executor import FaultExecutor
from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.registry import PluginRegistry
from asuran.core.types import (
    ExperimentStatus,
    FaultConfig,
    FaultResult,
    ParameterSpec,
    Phase,
    Severity,
)
from asuran.safety.rollback import RollbackManager


# ---------------------------------------------------------------------------
# FaultConfig / FaultResult / ParameterSpec
# ---------------------------------------------------------------------------


class TestFaultConfig:
    def test_creation_defaults(self):
        cfg = FaultConfig(params={"key": "val"})
        assert cfg.params == {"key": "val"}
        assert cfg.duration is None
        assert cfg.dry_run is False
        assert cfg.target_scope is None
        assert cfg.tags == []
        assert cfg.experiment_name is None

    def test_creation_with_all_fields(self):
        cfg = FaultConfig(
            params={"latency": 100},
            duration=30.0,
            dry_run=True,
            target_scope={"host": "web01"},
            tags=["canary", "prod"],
            experiment_name="exp-001",
        )
        assert cfg.params["latency"] == 100
        assert cfg.duration == 30.0
        assert cfg.dry_run is True
        assert cfg.target_scope == {"host": "web01"}
        assert cfg.tags == ["canary", "prod"]
        assert cfg.experiment_name == "exp-001"


class TestFaultResult:
    def test_creation_defaults(self):
        r = FaultResult(
            fault_id="abc",
            success=True,
            phase=Phase.ACTIVE,
            start_time=1000.0,
        )
        assert r.fault_id == "abc"
        assert r.success is True
        assert r.phase == Phase.ACTIVE
        assert r.start_time == 1000.0
        assert r.end_time is None
        assert r.metrics == {}
        assert r.error is None
        assert r.rollback_success is None
        assert r.domain == ""
        assert r.fault_type == ""

    def test_creation_full(self):
        r = FaultResult(
            fault_id="xyz",
            success=False,
            phase=Phase.FAILED,
            start_time=1000.0,
            end_time=1010.0,
            metrics={"cpu": 95.0},
            error="timeout",
            rollback_success=True,
            domain="net",
            fault_type="delay",
        )
        assert r.error == "timeout"
        assert r.metrics["cpu"] == 95.0
        assert r.rollback_success is True
        assert r.domain == "net"


class TestParameterSpec:
    def test_defaults(self):
        p = ParameterSpec(name="iface")
        assert p.name == "iface"
        assert p.type is str
        assert p.required is False
        assert p.default is None
        assert p.help == ""
        assert p.choices is None
        assert p.unit is None
        assert p.positional is False

    def test_fully_specified(self):
        p = ParameterSpec(
            name="latency",
            type=int,
            required=True,
            default=100,
            help="Latency in ms",
            choices=[50, 100, 200],
            unit="ms",
            positional=True,
        )
        assert p.type is int
        assert p.required is True
        assert p.default == 100
        assert p.choices == [50, 100, 200]
        assert p.unit == "ms"
        assert p.positional is True


# ---------------------------------------------------------------------------
# Enums
# ---------------------------------------------------------------------------


class TestSeverity:
    def test_members(self):
        members = list(Severity)
        assert len(members) == 4
        assert Severity.LOW in members
        assert Severity.MEDIUM in members
        assert Severity.HIGH in members
        assert Severity.CRITICAL in members

    def test_ordering(self):
        order = list(Severity)
        assert order.index(Severity.LOW) < order.index(Severity.CRITICAL)


class TestPhase:
    def test_members(self):
        members = list(Phase)
        assert Phase.PENDING in members
        assert Phase.VALIDATING in members
        assert Phase.PRE_CHECK in members
        assert Phase.INJECTING in members
        assert Phase.ACTIVE in members
        assert Phase.ROLLING_BACK in members
        assert Phase.COMPLETED in members
        assert Phase.FAILED in members


class TestExperimentStatus:
    def test_values(self):
        assert ExperimentStatus.PENDING.value == "pending"
        assert ExperimentStatus.RUNNING.value == "running"
        assert ExperimentStatus.COMPLETED.value == "completed"
        assert ExperimentStatus.FAILED.value == "failed"
        assert ExperimentStatus.ABORTED.value == "aborted"


# ---------------------------------------------------------------------------
# PluginRegistry
# ---------------------------------------------------------------------------


class _MockFault(FaultType):
    """A minimal FaultType for testing."""

    @property
    def name(self) -> str:
        return "mock-fault"

    @property
    def description(self) -> str:
        return "A mock fault for testing"

    @property
    def severity(self) -> Severity:
        return Severity.LOW

    def parameters(self) -> list[ParameterSpec]:
        return []

    def validate(self, config: FaultConfig) -> list[str]:
        return []

    def inject(self, config: FaultConfig, context: Any) -> str:
        return str(uuid.uuid4())[:8]

    def rollback(self, fault_id: str, context: Any) -> None:
        pass


class _MockDomain(ChaosDomain):
    """A minimal ChaosDomain for testing."""

    @property
    def name(self) -> str:
        return "mock"

    @property
    def description(self) -> str:
        return "A mock domain for testing"

    def fault_types(self) -> list[FaultType]:
        return [_MockFault()]


class TestPluginRegistry:
    def test_register_and_get_domain(self):
        reg = PluginRegistry()
        domain = _MockDomain()
        reg.register(domain)
        assert reg.get_domain("mock") is domain
        assert reg.get_domain("nonexistent") is None

    def test_register_non_domain_raises(self):
        reg = PluginRegistry()
        with pytest.raises(PluginError):
            reg.register("not a domain")  # type: ignore

    def test_get_fault_type(self):
        reg = PluginRegistry()
        reg.register(_MockDomain())
        ft = reg.get_fault_type("mock", "mock-fault")
        assert ft is not None
        assert ft.name == "mock-fault"

    def test_get_fault_type_missing_domain(self):
        reg = PluginRegistry()
        assert reg.get_fault_type("nope", "mock-fault") is None

    def test_get_fault_type_missing_fault(self):
        reg = PluginRegistry()
        reg.register(_MockDomain())
        assert reg.get_fault_type("mock", "nonexistent") is None

    def test_domain_names(self):
        reg = PluginRegistry()
        reg.register(_MockDomain())
        assert "mock" in reg.domain_names()

    def test_command_tree(self):
        reg = PluginRegistry()
        reg.register(_MockDomain())
        tree = reg.command_tree()
        assert "mock" in tree
        assert "mock-fault" in tree["mock"]

    def test_all_domains(self):
        reg = PluginRegistry()
        d = _MockDomain()
        reg.register(d)
        assert d in reg.all_domains()


# ---------------------------------------------------------------------------
# ExecutionContext
# ---------------------------------------------------------------------------


class TestExecutionContext:
    def test_dry_run_returns_zero(self):
        ctx = ExecutionContext(dry_run=True)
        result = ctx.run_cmd(["echo", "hello"])
        assert isinstance(result, subprocess.CompletedProcess)
        assert result.returncode == 0
        assert result.stdout == ""

    def test_check_tool_finds_python(self):
        ctx = ExecutionContext()
        # python3 is almost certainly on PATH in the test environment
        assert ctx.check_tool("python3") is True

    def test_check_tool_missing(self):
        ctx = ExecutionContext()
        assert ctx.check_tool("asuran_nonexistent_tool_xyz") is False

    def test_default_fields(self):
        ctx = ExecutionContext()
        assert ctx.dry_run is False
        assert ctx.scope == {}
        assert ctx.verbose == 0
        assert ctx.experiment_id is None


# ---------------------------------------------------------------------------
# FaultExecutor
# ---------------------------------------------------------------------------


class _ValidatingMockFault(FaultType):
    """A mock that fails validation if 'fail' is in params."""

    @property
    def name(self) -> str:
        return "val-mock"

    @property
    def description(self) -> str:
        return "Validating mock fault"

    @property
    def severity(self) -> Severity:
        return Severity.LOW

    def parameters(self) -> list[ParameterSpec]:
        return []

    def validate(self, config: FaultConfig) -> list[str]:
        if config.params.get("fail"):
            return ["intentional validation error"]
        return []

    def inject(self, config: FaultConfig, context: Any) -> str:
        return "vmock-id"

    def rollback(self, fault_id: str, context: Any) -> None:
        pass


class TestFaultExecutor:
    def test_execute_dry_run_success(self, tmp_path: Path):
        rm = RollbackManager(state_dir=tmp_path / ".asuran")
        ctx = ExecutionContext(rollback_manager=rm, dry_run=True)
        executor = FaultExecutor(ctx)
        fault = _MockFault()
        config = FaultConfig(params={}, dry_run=True)

        result = executor.execute(fault, config)
        assert result.success is True
        assert result.phase == Phase.ACTIVE

    def test_execute_validation_error(self, tmp_path: Path):
        rm = RollbackManager(state_dir=tmp_path / ".asuran")
        ctx = ExecutionContext(rollback_manager=rm, dry_run=True)
        executor = FaultExecutor(ctx)
        fault = _ValidatingMockFault()
        config = FaultConfig(params={"fail": True}, dry_run=True)

        result = executor.execute(fault, config)
        assert result.success is False
        assert result.phase == Phase.FAILED
        assert "intentional validation error" in result.error

    def test_active_count(self, tmp_path: Path):
        rm = RollbackManager(state_dir=tmp_path / ".asuran")
        ctx = ExecutionContext(rollback_manager=rm, dry_run=True)
        executor = FaultExecutor(ctx)
        fault = _MockFault()

        assert executor.active_count() == 0
        executor.execute(fault, FaultConfig(params={}, dry_run=True))
        assert executor.active_count() == 1

    def test_rollback_all(self, tmp_path: Path):
        rm = RollbackManager(state_dir=tmp_path / ".asuran")
        ctx = ExecutionContext(rollback_manager=rm, dry_run=True)
        executor = FaultExecutor(ctx)
        fault = _MockFault()

        executor.execute(fault, FaultConfig(params={}, dry_run=True))
        executor.execute(fault, FaultConfig(params={}, dry_run=True))
        assert executor.active_count() == 2

        count = executor.rollback_all()
        assert count == 2
        assert executor.active_count() == 0

    def test_rollback_one_unknown_id(self, tmp_path: Path):
        rm = RollbackManager(state_dir=tmp_path / ".asuran")
        ctx = ExecutionContext(rollback_manager=rm, dry_run=True)
        executor = FaultExecutor(ctx)
        assert executor.rollback_one("nonexistent") is False


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class TestErrors:
    def test_asuran_error_is_exception(self):
        assert issubclass(AsuranError, Exception)

    def test_plugin_error_inherits(self):
        assert issubclass(PluginError, AsuranError)

    def test_validation_error_carries_errors(self):
        e = ValidationError(["err1", "err2"])
        assert e.errors == ["err1", "err2"]
        assert "err1" in str(e)
        assert "err2" in str(e)
