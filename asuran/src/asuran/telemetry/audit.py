"""Command audit trail."""
from __future__ import annotations

import time
from dataclasses import dataclass, field


@dataclass
class AuditEntry:
    timestamp: float
    command: str
    result: str = ""
    fault_id: str = ""
    domain: str = ""


class AuditTrail:
    """Append-only audit log of all commands executed."""

    def __init__(self) -> None:
        self._entries: list[AuditEntry] = []

    def record(self, command: str, result: str = "", fault_id: str = "", domain: str = "") -> None:
        self._entries.append(AuditEntry(
            timestamp=time.time(),
            command=command,
            result=result,
            fault_id=fault_id,
            domain=domain,
        ))

    def last(self, n: int = 20) -> list[AuditEntry]:
        return self._entries[-n:]

    def all(self) -> list[AuditEntry]:
        return list(self._entries)

    def count(self) -> int:
        return len(self._entries)
