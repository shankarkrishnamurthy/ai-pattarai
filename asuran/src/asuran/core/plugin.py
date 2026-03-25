"""Abstract base classes for the Asuran plugin system.

Three-tier hierarchy:
    ChaosDomain (e.g., "network")
      -> FaultType (e.g., "delay")
           -> FaultInstance (running instance with parameters)
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any

from asuran.core.types import (
    FaultConfig,
    ParameterSpec,
    Phase,
    Severity,
)


class FaultType(ABC):
    """A specific kind of fault within a domain."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Short name used in CLI, e.g., 'delay', 'loss'."""
        ...

    @property
    @abstractmethod
    def description(self) -> str:
        """Human-readable one-liner."""
        ...

    @property
    @abstractmethod
    def severity(self) -> Severity:
        """Default severity classification."""
        ...

    @abstractmethod
    def parameters(self) -> list[ParameterSpec]:
        """Declare accepted parameters."""
        ...

    @abstractmethod
    def validate(self, config: FaultConfig) -> list[str]:
        """Return list of validation errors (empty = valid)."""
        ...

    @abstractmethod
    def inject(self, config: FaultConfig, context: Any) -> str:
        """Apply the fault. Return a fault_id (UUID).

        Must register rollback action with context.rollback_manager.
        """
        ...

    @abstractmethod
    def rollback(self, fault_id: str, context: Any) -> None:
        """Undo the fault. Must be idempotent."""
        ...

    def status(self, fault_id: str) -> Phase:
        """Check current phase of a fault instance."""
        return Phase.ACTIVE

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        """Verify prerequisites. Return warnings."""
        return []

    def collect_metrics(self, fault_id: str) -> dict[str, Any]:
        """Collect domain-specific metrics during fault."""
        return {}


class ChaosDomain(ABC):
    """A chaos domain plugin (e.g., network, cpu, disk)."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Domain name used as CLI top-level command."""
        ...

    @property
    @abstractmethod
    def description(self) -> str:
        """Domain description for help text."""
        ...

    @abstractmethod
    def fault_types(self) -> list[FaultType]:
        """Return all fault types in this domain."""
        ...

    def required_capabilities(self) -> list[str]:
        """Linux capabilities required."""
        return []

    def required_tools(self) -> list[str]:
        """External tools required."""
        return []
