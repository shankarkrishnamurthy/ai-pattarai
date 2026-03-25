"""Execution context passed to every fault injection."""
from __future__ import annotations

import logging
import shutil
import subprocess
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Optional

if TYPE_CHECKING:
    from asuran.safety.rollback import RollbackManager
    from asuran.telemetry.logger import AsuranLogger

logger = logging.getLogger(__name__)


@dataclass
class ExecutionContext:
    """Carries runtime state through fault injection lifecycle."""

    rollback_manager: Any = None  # RollbackManager
    asuran_logger: Any = None     # AsuranLogger
    dry_run: bool = False
    scope: dict[str, Any] = field(default_factory=dict)
    experiment_id: Optional[str] = None
    verbose: int = 0

    def run_cmd(
        self,
        cmd: list[str],
        check: bool = True,
        timeout: float = 30.0,
        capture: bool = True,
    ) -> subprocess.CompletedProcess:
        """Execute a system command with logging."""
        logger.debug("Running: %s", " ".join(cmd))
        if self.dry_run:
            logger.info("[DRY RUN] Would run: %s", " ".join(cmd))
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
        return subprocess.run(
            cmd,
            check=check,
            timeout=timeout,
            capture_output=capture,
            text=True,
        )

    def check_tool(self, name: str) -> bool:
        """Check if a system tool is available."""
        return shutil.which(name) is not None

    def log(self, message: str, level: str = "INFO") -> None:
        getattr(logger, level.lower(), logger.info)(message)
