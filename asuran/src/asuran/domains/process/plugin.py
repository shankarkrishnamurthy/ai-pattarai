"""Process chaos domain — kill, freeze, zombie faults."""
from __future__ import annotations

import logging
import os
import signal
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Phase, Severity

logger = logging.getLogger(__name__)


class KillFault(FaultType):
    """Send a signal to a process (default SIGTERM). Irreversible."""

    _killed: dict[str, dict[str, Any]]

    def __init__(self) -> None:
        self._killed = {}

    @property
    def name(self) -> str:
        return "kill"

    @property
    def description(self) -> str:
        return "Send a signal to a process to terminate or disrupt it"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="pid",
                type=int,
                required=False,
                default=None,
                help="PID of the target process",
            ),
            ParameterSpec(
                name="name",
                type=str,
                required=False,
                default=None,
                help="Process name to kill (resolved via pgrep)",
            ),
            ParameterSpec(
                name="signal",
                type=str,
                required=False,
                default="SIGTERM",
                help="Signal to send (e.g. SIGTERM, SIGKILL, SIGHUP)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        pid = config.params.get("pid")
        name = config.params.get("name")

        if pid is None and name is None:
            errors.append("Either 'pid' or 'name' must be specified")
        if pid is not None and name is not None:
            errors.append("Specify only one of 'pid' or 'name', not both")
        if pid is not None and (not isinstance(pid, int) or pid < 1):
            errors.append("'pid' must be a positive integer")
        if name is not None and (not isinstance(name, str) or not name):
            errors.append("'name' must be a non-empty string")

        sig_name = config.params.get("signal", "SIGTERM")
        if not hasattr(signal, sig_name):
            errors.append(f"Unknown signal: '{sig_name}'")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        pid: int | None = config.params.get("pid")
        name: str | None = config.params.get("name")
        sig_name: str = config.params.get("signal", "SIGTERM")
        sig_num: int = getattr(signal, sig_name)

        # Resolve PID from process name if needed
        if pid is None and name is not None:
            result = context.run_cmd(["pgrep", "-x", name], check=False)
            if result.returncode != 0 or not result.stdout.strip():
                logger.error("[%s] No process found with name '%s'", fault_id, name)
                raise RuntimeError(f"No process found with name '{name}'")
            pid = int(result.stdout.strip().splitlines()[0])

        logger.info("[%s] Sending %s to PID %d", fault_id, sig_name, pid)

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would send %s to PID %d", sig_name, pid)
            self._killed[fault_id] = {"pid": pid, "signal": sig_name}
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"No-op rollback for kill (PID {pid}, {sig_name}) — irreversible",
                domain="proc",
                fault_type="kill",
            )
            return fault_id

        os.kill(pid, sig_num)
        self._killed[fault_id] = {"pid": pid, "signal": sig_name}
        logger.info("[%s] Sent %s to PID %d", fault_id, sig_name, pid)

        # Kill is irreversible — push a no-op rollback for tracking
        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"No-op rollback for kill (PID {pid}, {sig_name}) — irreversible",
            domain="proc",
            fault_type="kill",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        entry = self._killed.pop(fault_id, None)
        if entry:
            logger.info(
                "[%s] Kill is irreversible — no-op rollback (PID %d, %s)",
                fault_id, entry["pid"], entry["signal"],
            )
        else:
            logger.info("[%s] No kill record to roll back", fault_id)

    def status(self, fault_id: str) -> Phase:
        return Phase.COMPLETED


class FreezeFault(FaultType):
    """Freeze a process by sending SIGSTOP; rollback sends SIGCONT."""

    _frozen_pids: dict[str, int]

    def __init__(self) -> None:
        self._frozen_pids = {}

    @property
    def name(self) -> str:
        return "freeze"

    @property
    def description(self) -> str:
        return "Freeze a process with SIGSTOP (resume with SIGCONT on rollback)"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="pid",
                type=int,
                required=True,
                help="PID of the process to freeze",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        pid = config.params.get("pid")
        if pid is None:
            errors.append("'pid' is required")
        elif not isinstance(pid, int) or pid < 1:
            errors.append("'pid' must be a positive integer")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        pid: int = config.params["pid"]

        logger.info("[%s] Freezing PID %d (SIGSTOP)", fault_id, pid)

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would send SIGSTOP to PID %d", pid)
            self._frozen_pids[fault_id] = pid
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Unfreeze PID {pid} (SIGCONT)",
                domain="proc",
                fault_type="freeze",
            )
            return fault_id

        os.kill(pid, signal.SIGSTOP)
        self._frozen_pids[fault_id] = pid
        logger.info("[%s] Froze PID %d", fault_id, pid)

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Unfreeze PID {pid} (SIGCONT)",
            domain="proc",
            fault_type="freeze",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        pid = self._frozen_pids.pop(fault_id, None)
        if pid is None:
            logger.info("[%s] No frozen process to unfreeze (already cleaned up)", fault_id)
            return

        try:
            os.kill(pid, signal.SIGCONT)
            logger.info("[%s] Unfroze PID %d (SIGCONT)", fault_id, pid)
        except ProcessLookupError:
            logger.warning("[%s] PID %d no longer exists — cannot unfreeze", fault_id, pid)
        except PermissionError:
            logger.error("[%s] Permission denied sending SIGCONT to PID %d", fault_id, pid)

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._frozen_pids:
            return Phase.ACTIVE
        return Phase.COMPLETED


class ZombieFault(FaultType):
    """Create zombie processes by forking children that exit without being waited on."""

    _children: dict[str, list[int]]

    def __init__(self) -> None:
        self._children = {}

    @property
    def name(self) -> str:
        return "zombie"

    @property
    def description(self) -> str:
        return "Create zombie processes (exited children not reaped by parent)"

    @property
    def severity(self) -> Severity:
        return Severity.LOW

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="count",
                type=int,
                required=True,
                default=10,
                help="Number of zombie processes to create",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        count = config.params.get("count", 10)
        if not isinstance(count, int) or count < 1:
            errors.append("'count' must be a positive integer")
        if isinstance(count, int) and count > 10_000:
            errors.append("'count' must be <= 10000 to avoid exhausting PID space")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        count: int = config.params.get("count", 10)

        logger.info("[%s] Creating %d zombie processes", fault_id, count)

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would fork %d children and let them exit without wait()", count)
            self._children[fault_id] = []
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Reap {count} zombie processes",
                domain="proc",
                fault_type="zombie",
            )
            return fault_id

        child_pids: list[int] = []
        for _ in range(count):
            pid = os.fork()
            if pid == 0:
                # Child process — exit immediately to become a zombie
                os._exit(0)
            else:
                child_pids.append(pid)

        self._children[fault_id] = child_pids
        logger.info(
            "[%s] Created %d zombie processes (PIDs: %s...)",
            fault_id, len(child_pids),
            ", ".join(str(p) for p in child_pids[:5]),
        )

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Reap {count} zombie processes",
            domain="proc",
            fault_type="zombie",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        child_pids = self._children.pop(fault_id, None)
        if child_pids is None:
            logger.info("[%s] No zombie children to reap (already cleaned up)", fault_id)
            return

        reaped = 0
        for pid in child_pids:
            try:
                os.waitpid(pid, os.WNOHANG)
                reaped += 1
            except ChildProcessError:
                # Already reaped or not a child
                pass

        logger.info("[%s] Reaped %d/%d zombie processes", fault_id, reaped, len(child_pids))

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._children:
            return Phase.ACTIVE
        return Phase.COMPLETED


class ProcessDomain(ChaosDomain):
    """Process chaos domain."""

    @property
    def name(self) -> str:
        return "proc"

    @property
    def description(self) -> str:
        return "Process chaos: kill, signal, freeze, zombie"

    def fault_types(self) -> list[FaultType]:
        return [KillFault(), FreezeFault(), ZombieFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_KILL", "CAP_SYS_PTRACE"]

    def required_tools(self) -> list[str]:
        return ["pgrep"]


def create_domain() -> ChaosDomain:
    """Entry point for the plugin loader."""
    return ProcessDomain()
