"""Active fault tracker."""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Optional


@dataclass
class ActiveFaultInfo:
    fault_id: str
    domain: str
    fault_type: str
    config: dict[str, Any]
    start_time: float
    duration: Optional[float] = None
    tags: list[str] = field(default_factory=list)

    @property
    def elapsed(self) -> float:
        return time.time() - self.start_time

    @property
    def remaining(self) -> Optional[float]:
        if self.duration is None:
            return None
        r = self.duration - self.elapsed
        return max(0.0, r)


class ActiveFaultTracker:
    """Tracks all currently active faults."""

    def __init__(self) -> None:
        self._faults: dict[str, ActiveFaultInfo] = {}

    def add(self, info: ActiveFaultInfo) -> None:
        self._faults[info.fault_id] = info

    def remove(self, fault_id: str) -> Optional[ActiveFaultInfo]:
        return self._faults.pop(fault_id, None)

    def get(self, fault_id: str) -> Optional[ActiveFaultInfo]:
        return self._faults.get(fault_id)

    def all(self) -> list[ActiveFaultInfo]:
        return list(self._faults.values())

    def count(self) -> int:
        return len(self._faults)

    def clear(self) -> None:
        self._faults.clear()
