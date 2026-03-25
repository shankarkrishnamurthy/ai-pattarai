"""Blast radius controls and duration enforcement."""
from __future__ import annotations

import logging
from dataclasses import dataclass, field

from asuran.core.errors import BlastRadiusError, DurationError, SeverityError
from asuran.core.plugin import FaultType
from asuran.core.types import FaultConfig, Severity

logger = logging.getLogger(__name__)


@dataclass
class SafetyConfig:
    max_default_duration: float = 300.0
    max_absolute_duration: float = 3600.0
    max_concurrent_faults: int = 10
    max_severity: Severity = Severity.HIGH
    confirm_above_severity: Severity = Severity.MEDIUM
    disabled_domains: list[str] = field(default_factory=list)
    protected_interfaces: list[str] = field(default_factory=lambda: ["lo"])
    protected_processes: list[str] = field(default_factory=lambda: ["systemd", "sshd", "asuran"])
    protected_paths: list[str] = field(default_factory=lambda: ["/boot", "/"])
    dry_run_default: bool = False


class BlastRadiusGuard:
    """Enforces safety limits before fault injection."""

    def __init__(self, config: SafetyConfig | None = None) -> None:
        self.config = config or SafetyConfig()

    def check(self, fault_type: FaultType, fault_config: FaultConfig, active_count: int) -> None:
        # Check max concurrent
        if active_count >= self.config.max_concurrent_faults:
            raise BlastRadiusError(
                f"Max concurrent faults ({self.config.max_concurrent_faults}) reached"
            )

        # Check severity
        severity_order = list(Severity)
        max_idx = severity_order.index(self.config.max_severity)
        fault_idx = severity_order.index(fault_type.severity)
        if fault_idx > max_idx:
            raise SeverityError(
                f"Severity {fault_type.severity.name} exceeds max allowed {self.config.max_severity.name}"
            )

        # Check duration
        if fault_config.duration and fault_config.duration > self.config.max_absolute_duration:
            raise DurationError(
                f"Duration {fault_config.duration}s exceeds max {self.config.max_absolute_duration}s"
            )

        # Apply default duration if none specified
        if not fault_config.duration and not fault_config.dry_run:
            fault_config.duration = self.config.max_default_duration

    def needs_confirmation(self, fault_type: FaultType) -> bool:
        severity_order = list(Severity)
        confirm_idx = severity_order.index(self.config.confirm_above_severity)
        fault_idx = severity_order.index(fault_type.severity)
        return fault_idx >= confirm_idx

    def is_protected_interface(self, iface: str) -> bool:
        return iface in self.config.protected_interfaces

    def is_protected_process(self, name: str) -> bool:
        return name in self.config.protected_processes

    def is_protected_path(self, path: str) -> bool:
        return path in self.config.protected_paths

    def is_domain_disabled(self, domain: str) -> bool:
        return domain in self.config.disabled_domains
