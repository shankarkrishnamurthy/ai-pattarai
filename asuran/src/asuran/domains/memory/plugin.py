"""Memory chaos domain — pressure, OOM score adjustment, swap tuning."""
from __future__ import annotations

import logging
import mmap
import os
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Phase, Severity

logger = logging.getLogger(__name__)


def _parse_size(size_str: str) -> int:
    """Parse a human-readable size string (e.g. '1G', '512M') to bytes."""
    size_str = size_str.strip().upper()
    multipliers = {"K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
    if size_str[-1] in multipliers:
        return int(float(size_str[:-1]) * multipliers[size_str[-1]])
    return int(size_str)


class PressureFault(FaultType):
    """Allocate anonymous memory via mmap to create memory pressure."""

    _allocations: dict[str, mmap.mmap]

    def __init__(self) -> None:
        self._allocations = {}

    @property
    def name(self) -> str:
        return "pressure"

    @property
    def description(self) -> str:
        return "Allocate anonymous memory to create memory pressure"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="percent",
                type=int,
                required=False,
                default=None,
                unit="%",
                help="Percentage of total memory to consume",
            ),
            ParameterSpec(
                name="size",
                type=str,
                required=False,
                default=None,
                help="Absolute size to allocate (e.g. '1G', '512M')",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        percent = config.params.get("percent")
        size = config.params.get("size")

        if percent is None and size is None:
            errors.append("Either 'percent' or 'size' must be specified")
        if percent is not None and size is not None:
            errors.append("Specify only one of 'percent' or 'size', not both")
        if percent is not None:
            if not isinstance(percent, int) or not (1 <= percent <= 100):
                errors.append("'percent' must be an integer between 1 and 100")
        if size is not None:
            try:
                _parse_size(size)
            except (ValueError, IndexError):
                errors.append("'size' must be a valid size string (e.g. '1G', '512M')")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        percent = config.params.get("percent")
        size_str = config.params.get("size")

        # Determine allocation size in bytes
        if size_str is not None:
            alloc_bytes = _parse_size(size_str)
        else:
            # Read total memory from /proc/meminfo
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        total_kb = int(line.split()[1])
                        break
                else:
                    total_kb = 0
            total_bytes = total_kb * 1024
            alloc_bytes = int(total_bytes * percent / 100)

        logger.info(
            "[%s] Injecting memory pressure: %d bytes (%.1f MB)",
            fault_id, alloc_bytes, alloc_bytes / (1024 * 1024),
        )

        if config.dry_run or context.dry_run:
            logger.info(
                "[DRY RUN] Would allocate %d bytes of anonymous memory",
                alloc_bytes,
            )
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Release {alloc_bytes} bytes of memory pressure",
                domain="mem",
                fault_type="pressure",
            )
            return fault_id

        # Allocate anonymous memory via mmap
        mm = mmap.mmap(-1, alloc_bytes, mmap.MAP_ANONYMOUS | mmap.MAP_PRIVATE)

        # Write to every page to force the kernel to commit the memory
        page_size = os.sysconf("SC_PAGE_SIZE")
        for offset in range(0, alloc_bytes, page_size):
            mm[offset] = 0xFF

        self._allocations[fault_id] = mm
        logger.info("[%s] Allocated and committed %d bytes", fault_id, alloc_bytes)

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Release {alloc_bytes} bytes of memory pressure",
            domain="mem",
            fault_type="pressure",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        mm = self._allocations.pop(fault_id, None)
        if mm is None:
            logger.info("[%s] No memory allocation to release (already cleaned up)", fault_id)
            return

        mm.close()
        logger.info("[%s] Released memory allocation", fault_id)

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._allocations:
            return Phase.ACTIVE
        return Phase.COMPLETED


class OomFault(FaultType):
    """Adjust /proc/{pid}/oom_score_adj to influence OOM killer targeting."""

    _original_scores: dict[str, tuple[int, int]]  # fault_id -> (pid, original_score)

    def __init__(self) -> None:
        self._original_scores = {}

    @property
    def name(self) -> str:
        return "oom"

    @property
    def description(self) -> str:
        return "Adjust OOM killer score for a process"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="pid",
                type=int,
                required=True,
                help="PID of the target process",
            ),
            ParameterSpec(
                name="score",
                type=int,
                required=True,
                help="OOM score adjustment (-1000 to 1000; 1000 = always kill first)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        pid = config.params.get("pid")
        if pid is None:
            errors.append("'pid' is required")
        elif not isinstance(pid, int) or pid < 1:
            errors.append("'pid' must be a positive integer")

        score = config.params.get("score")
        if score is None:
            errors.append("'score' is required")
        elif not isinstance(score, int) or not (-1000 <= score <= 1000):
            errors.append("'score' must be an integer between -1000 and 1000")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        pid: int = config.params["pid"]
        score: int = config.params["score"]
        oom_path = f"/proc/{pid}/oom_score_adj"

        logger.info("[%s] Setting OOM score for PID %d to %d", fault_id, pid, score)

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would write %d to %s", score, oom_path)
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Restore OOM score for PID {pid}",
                domain="mem",
                fault_type="oom",
            )
            return fault_id

        # Read original score
        result = context.run_cmd(["cat", oom_path])
        original_score = int(result.stdout.strip())
        self._original_scores[fault_id] = (pid, original_score)

        # Write new score
        context.run_cmd(["sh", "-c", f"echo {score} > {oom_path}"])
        logger.info(
            "[%s] Changed OOM score for PID %d from %d to %d",
            fault_id, pid, original_score, score,
        )

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Restore OOM score for PID {pid} to {original_score}",
            domain="mem",
            fault_type="oom",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        entry = self._original_scores.pop(fault_id, None)
        if entry is None:
            logger.info("[%s] No OOM score to restore (already cleaned up)", fault_id)
            return

        pid, original_score = entry
        oom_path = f"/proc/{pid}/oom_score_adj"
        context.run_cmd(["sh", "-c", f"echo {original_score} > {oom_path}"], check=False)
        logger.info("[%s] Restored OOM score for PID %d to %d", fault_id, pid, original_score)

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._original_scores:
            return Phase.ACTIVE
        return Phase.COMPLETED


class SwapFault(FaultType):
    """Modify /proc/sys/vm/swappiness to influence kernel swap behaviour."""

    _original_swappiness: dict[str, int]

    def __init__(self) -> None:
        self._original_swappiness = {}

    @property
    def name(self) -> str:
        return "swap"

    @property
    def description(self) -> str:
        return "Modify system swappiness value"

    @property
    def severity(self) -> Severity:
        return Severity.LOW

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="swappiness",
                type=int,
                required=True,
                help="Swappiness value (0-200)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        swappiness = config.params.get("swappiness")
        if swappiness is None:
            errors.append("'swappiness' is required")
        elif not isinstance(swappiness, int) or not (0 <= swappiness <= 200):
            errors.append("'swappiness' must be an integer between 0 and 200")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        swappiness: int = config.params["swappiness"]
        swappiness_path = "/proc/sys/vm/swappiness"

        logger.info("[%s] Setting swappiness to %d", fault_id, swappiness)

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would write %d to %s", swappiness, swappiness_path)
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Restore swappiness value",
                domain="mem",
                fault_type="swap",
            )
            return fault_id

        # Read original value
        result = context.run_cmd(["cat", swappiness_path])
        original = int(result.stdout.strip())
        self._original_swappiness[fault_id] = original

        # Write new value
        context.run_cmd(["sh", "-c", f"echo {swappiness} > {swappiness_path}"])
        logger.info("[%s] Changed swappiness from %d to %d", fault_id, original, swappiness)

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Restore swappiness from {swappiness} to {original}",
            domain="mem",
            fault_type="swap",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        original = self._original_swappiness.pop(fault_id, None)
        if original is None:
            logger.info("[%s] No swappiness to restore (already cleaned up)", fault_id)
            return

        swappiness_path = "/proc/sys/vm/swappiness"
        context.run_cmd(["sh", "-c", f"echo {original} > {swappiness_path}"], check=False)
        logger.info("[%s] Restored swappiness to %d", fault_id, original)

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._original_swappiness:
            return Phase.ACTIVE
        return Phase.COMPLETED


class MemoryDomain(ChaosDomain):
    """Memory chaos domain."""

    @property
    def name(self) -> str:
        return "mem"

    @property
    def description(self) -> str:
        return "Memory chaos: pressure, OOM, swap"

    def fault_types(self) -> list[FaultType]:
        return [PressureFault(), OomFault(), SwapFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_ADMIN"]

    def required_tools(self) -> list[str]:
        return []


def create_domain() -> ChaosDomain:
    """Entry point for the plugin loader."""
    return MemoryDomain()
