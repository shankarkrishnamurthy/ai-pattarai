"""Core types, enums, and dataclasses for Asuran."""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Any, Optional


class Severity(Enum):
    LOW = auto()
    MEDIUM = auto()
    HIGH = auto()
    CRITICAL = auto()


class Phase(Enum):
    PENDING = auto()
    VALIDATING = auto()
    PRE_CHECK = auto()
    INJECTING = auto()
    ACTIVE = auto()
    ROLLING_BACK = auto()
    COMPLETED = auto()
    FAILED = auto()


class ExperimentStatus(Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"
    ABORTED = "aborted"


class RollbackStrategy(Enum):
    IMMEDIATE = "immediate"
    END = "end"
    MANUAL = "manual"


class OnFailure(Enum):
    ABORT = "abort"
    CONTINUE = "continue"
    ROLLBACK_AND_CONTINUE = "rollback-and-continue"


@dataclass
class ParameterSpec:
    """Declares a parameter that a FaultType accepts."""
    name: str
    type: type = str
    required: bool = False
    default: Any = None
    help: str = ""
    choices: Optional[list] = None
    unit: Optional[str] = None
    positional: bool = False


@dataclass
class FaultConfig:
    """Validated, immutable configuration for a fault instance."""
    params: dict[str, Any]
    duration: Optional[float] = None
    dry_run: bool = False
    target_scope: Optional[dict] = None
    tags: list[str] = field(default_factory=list)
    experiment_name: Optional[str] = None


@dataclass
class FaultResult:
    """Outcome of a fault injection."""
    fault_id: str
    success: bool
    phase: Phase
    start_time: float
    end_time: Optional[float] = None
    metrics: dict[str, Any] = field(default_factory=dict)
    error: Optional[str] = None
    rollback_success: Optional[bool] = None
    domain: str = ""
    fault_type: str = ""
