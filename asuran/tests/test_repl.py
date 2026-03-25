"""Tests for the REPL/CLI: _parse_value, _parse_command, _build_completer."""
from __future__ import annotations

from typing import Any
from unittest.mock import MagicMock

import pytest

from asuran.cli.repl import _build_completer, _parse_command, _parse_value
from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.registry import PluginRegistry
from asuran.core.types import FaultConfig, ParameterSpec, Severity


# ---------------------------------------------------------------------------
# _parse_value
# ---------------------------------------------------------------------------


class TestParseValue:
    def test_milliseconds(self):
        assert _parse_value("100ms", int) == 100

    def test_gigabytes(self):
        assert _parse_value("10G", int) == 10

    def test_megabytes(self):
        assert _parse_value("512M", int) == 512

    def test_plain_int(self):
        assert _parse_value("50", int) == 50

    def test_plain_float(self):
        assert _parse_value("3.14", float) == pytest.approx(3.14)

    def test_percent_suffix(self):
        assert _parse_value("75%", int) == 75

    def test_seconds_suffix(self):
        assert _parse_value("30s", float) == 30.0

    def test_kilobytes(self):
        assert _parse_value("1024K", int) == 1024

    def test_terabytes(self):
        assert _parse_value("2T", int) == 2

    def test_string_passthrough(self):
        assert _parse_value("eth0", str) == "eth0"

    def test_hours(self):
        assert _parse_value("2h", float) == 2.0

    def test_minutes(self):
        assert _parse_value("5m", float) == 5.0


# ---------------------------------------------------------------------------
# Helper domain for _parse_command tests
# ---------------------------------------------------------------------------


class _TestFault(FaultType):
    @property
    def name(self) -> str:
        return "delay"

    @property
    def description(self) -> str:
        return "Test delay"

    @property
    def severity(self) -> Severity:
        return Severity.LOW

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="interface", type=str, required=True, positional=True),
            ParameterSpec(name="latency", type=int, required=True),
            ParameterSpec(name="jitter", type=int, required=False, default=0),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        return []

    def inject(self, config: FaultConfig, context: Any) -> str:
        return "test-id"

    def rollback(self, fault_id: str, context: Any) -> None:
        pass


class _TestDomain(ChaosDomain):
    @property
    def name(self) -> str:
        return "net"

    @property
    def description(self) -> str:
        return "Test network domain"

    def fault_types(self) -> list[FaultType]:
        return [_TestFault()]


# ---------------------------------------------------------------------------
# _parse_command
# ---------------------------------------------------------------------------


class TestParseCommand:
    @pytest.fixture
    def registry(self) -> PluginRegistry:
        reg = PluginRegistry()
        reg.register(_TestDomain())
        return reg

    def test_valid_named_params(self, registry: PluginRegistry):
        tokens = ["net", "delay", "eth0", "--latency", "100"]
        ft, cfg = _parse_command(tokens, registry)
        assert ft is not None
        assert cfg is not None
        assert ft.name == "delay"
        assert cfg.params["interface"] == "eth0"
        assert cfg.params["latency"] == 100

    def test_valid_with_duration(self, registry: PluginRegistry):
        tokens = ["net", "delay", "eth0", "--latency", "50", "--duration", "30"]
        ft, cfg = _parse_command(tokens, registry)
        assert cfg.duration == 30.0

    def test_valid_with_dry_run(self, registry: PluginRegistry):
        tokens = ["net", "delay", "eth0", "--latency", "50", "--dry-run"]
        ft, cfg = _parse_command(tokens, registry)
        assert cfg.dry_run is True

    def test_valid_with_tag(self, registry: PluginRegistry):
        tokens = ["net", "delay", "eth0", "--latency", "50", "--tag", "canary"]
        ft, cfg = _parse_command(tokens, registry)
        assert "canary" in cfg.tags

    def test_default_applied(self, registry: PluginRegistry):
        tokens = ["net", "delay", "eth0", "--latency", "100"]
        ft, cfg = _parse_command(tokens, registry)
        assert cfg.params["jitter"] == 0  # default value

    def test_invalid_domain(self, registry: PluginRegistry):
        tokens = ["bogus", "delay", "eth0"]
        ft, cfg = _parse_command(tokens, registry)
        assert ft is None
        assert cfg is None

    def test_invalid_fault_name(self, registry: PluginRegistry):
        tokens = ["net", "bogus", "eth0"]
        ft, cfg = _parse_command(tokens, registry)
        assert ft is None
        assert cfg is None

    def test_too_few_tokens(self, registry: PluginRegistry):
        ft, cfg = _parse_command(["net"], registry)
        assert ft is None
        assert cfg is None

    def test_empty_tokens(self, registry: PluginRegistry):
        ft, cfg = _parse_command([], registry)
        assert ft is None
        assert cfg is None


# ---------------------------------------------------------------------------
# _build_completer
# ---------------------------------------------------------------------------


class TestBuildCompleter:
    def test_returns_word_completer(self):
        from prompt_toolkit.completion import WordCompleter

        reg = PluginRegistry()
        reg.register(_TestDomain())
        completer = _build_completer(reg)
        assert isinstance(completer, WordCompleter)

    def test_includes_domain_names(self):
        reg = PluginRegistry()
        reg.register(_TestDomain())
        completer = _build_completer(reg)
        assert "net" in completer.words

    def test_includes_fault_names(self):
        reg = PluginRegistry()
        reg.register(_TestDomain())
        completer = _build_completer(reg)
        assert "delay" in completer.words

    def test_includes_builtins(self):
        reg = PluginRegistry()
        reg.register(_TestDomain())
        completer = _build_completer(reg)
        for word in ["status", "recover", "exit", "quit", "help", "--dry-run"]:
            assert word in completer.words
