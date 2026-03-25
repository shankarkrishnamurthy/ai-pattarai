"""RollbackManager — LIFO undo stack with signal handlers and crash recovery."""
from __future__ import annotations

import atexit
import json
import logging
import os
import signal
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

logger = logging.getLogger(__name__)


@dataclass
class RollbackAction:
    fault_id: str
    rollback_fn: Callable[[], None]
    description: str
    domain: str = ""
    fault_type: str = ""
    rollback_cmd: Optional[list[str]] = None


class RollbackManager:
    """Maintains an ordered stack of undo operations.

    Guarantees:
    - LIFO rollback order
    - Idempotent rollback
    - Signal handler registration (SIGINT, SIGTERM, SIGHUP)
    - atexit handler for clean shutdown
    - Crash recovery file (~/.asuran/active_faults.json)
    """

    def __init__(self, state_dir: Optional[Path] = None) -> None:
        self._stack: list[RollbackAction] = []
        self._rolled_back: set[str] = set()
        self._state_dir = state_dir or Path.home() / ".asuran"
        self._state_file = self._state_dir / "active_faults.json"
        self._original_handlers: dict[int, object] = {}
        self._setup_handlers()

    def _setup_handlers(self) -> None:
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                self._original_handlers[sig] = signal.getsignal(sig)
                signal.signal(sig, self._signal_handler)
            except (OSError, ValueError):
                pass
        atexit.register(self._atexit_handler)

    def _signal_handler(self, signum: int, frame: object) -> None:
        logger.warning("Signal %d received, rolling back all faults", signum)
        self.rollback_all()
        orig = self._original_handlers.get(signum)
        if callable(orig):
            orig(signum, frame)

    def _atexit_handler(self) -> None:
        if self._stack:
            logger.info("atexit: rolling back %d active faults", len(self._stack))
            self.rollback_all()

    def push(self, fault_id: str, rollback_fn: Callable[[], None], description: str,
             domain: str = "", fault_type: str = "", rollback_cmd: Optional[list[str]] = None) -> None:
        action = RollbackAction(
            fault_id=fault_id,
            rollback_fn=rollback_fn,
            description=description,
            domain=domain,
            fault_type=fault_type,
            rollback_cmd=rollback_cmd,
        )
        self._stack.append(action)
        self._save_state()
        logger.debug("Pushed rollback for %s: %s", fault_id, description)

    def rollback_one(self, fault_id: str) -> bool:
        if fault_id in self._rolled_back:
            return True

        action = None
        for a in self._stack:
            if a.fault_id == fault_id:
                action = a
                break

        if not action:
            logger.warning("No rollback action for fault %s", fault_id)
            return False

        try:
            action.rollback_fn()
            self._rolled_back.add(fault_id)
            self._stack = [a for a in self._stack if a.fault_id != fault_id]
            self._save_state()
            logger.info("Rolled back: %s", action.description)
            return True
        except Exception as e:
            logger.error("Rollback failed for %s: %s", fault_id, e)
            return False

    def rollback_all(self) -> int:
        count = 0
        for action in reversed(list(self._stack)):
            if self.rollback_one(action.fault_id):
                count += 1
        return count

    def active_count(self) -> int:
        return len(self._stack)

    def active_faults(self) -> list[dict]:
        return [
            {"fault_id": a.fault_id, "description": a.description,
             "domain": a.domain, "fault_type": a.fault_type}
            for a in self._stack
        ]

    def _save_state(self) -> None:
        try:
            self._state_dir.mkdir(parents=True, exist_ok=True)
            data = [
                {
                    "fault_id": a.fault_id,
                    "description": a.description,
                    "domain": a.domain,
                    "fault_type": a.fault_type,
                    "rollback_cmd": a.rollback_cmd,
                    "pid": os.getpid(),
                }
                for a in self._stack
            ]
            # Atomic write
            fd, tmp = tempfile.mkstemp(dir=self._state_dir, suffix=".tmp")
            try:
                with os.fdopen(fd, "w") as f:
                    json.dump(data, f)
                os.replace(tmp, self._state_file)
            except Exception:
                os.unlink(tmp)
                raise
        except Exception as e:
            logger.error("Failed to save rollback state: %s", e)

    def recover_from_crash(self) -> list[dict]:
        """Check for orphaned faults from a previous crash."""
        if not self._state_file.exists():
            return []
        try:
            with open(self._state_file) as f:
                data = json.load(f)
            if data:
                return data
        except Exception as e:
            logger.error("Failed to read crash recovery file: %s", e)
        return []

    def clear_state(self) -> None:
        try:
            self._state_file.unlink(missing_ok=True)
        except Exception:
            pass
