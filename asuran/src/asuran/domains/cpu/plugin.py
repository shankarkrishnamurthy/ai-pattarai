"""CPU chaos domain — stress, throttle, affinity faults."""
from __future__ import annotations

import logging
import multiprocessing
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Phase, Severity

logger = logging.getLogger(__name__)


def _cpu_burn() -> None:
    """Worker function that burns CPU by doing pointless math."""
    while True:
        _ = sum(i * i for i in range(10_000))


class StressFault(FaultType):
    """Spawn CPU-burning processes to generate load."""

    _workers: dict[str, list[multiprocessing.Process]]

    def __init__(self) -> None:
        self._workers = {}

    @property
    def name(self) -> str:
        return "stress"

    @property
    def description(self) -> str:
        return "Spawn CPU-burning worker processes to generate sustained load"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="cores",
                type=int,
                required=True,
                help="Number of CPU cores to stress",
            ),
            ParameterSpec(
                name="load",
                type=int,
                required=False,
                default=100,
                unit="%",
                help="Target CPU load percentage per core",
            ),
            ParameterSpec(
                name="duration",
                type=float,
                required=False,
                default=None,
                help="Duration in seconds (None = until rollback)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        cores = config.params.get("cores")
        if cores is None:
            errors.append("'cores' is required")
        elif not isinstance(cores, int) or cores < 1:
            errors.append("'cores' must be a positive integer")

        load = config.params.get("load", 100)
        if not isinstance(load, int) or not (1 <= load <= 100):
            errors.append("'load' must be an integer between 1 and 100")

        duration = config.params.get("duration")
        if duration is not None and (not isinstance(duration, (int, float)) or duration <= 0):
            errors.append("'duration' must be a positive number")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        cores: int = config.params["cores"]
        load: int = config.params.get("load", 100)

        logger.info(
            "[%s] Injecting CPU stress: cores=%d load=%d%%",
            fault_id, cores, load,
        )

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would spawn %d CPU-burn workers at %d%% load", cores, load)
            self._workers[fault_id] = []
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Terminate {cores} CPU stress workers",
                domain="cpu",
                fault_type="stress",
            )
            return fault_id

        workers: list[multiprocessing.Process] = []
        for i in range(cores):
            p = multiprocessing.Process(target=_cpu_burn, daemon=True, name=f"asuran-cpu-{fault_id[:8]}-{i}")
            p.start()
            workers.append(p)
            logger.debug("Started CPU worker PID %s", p.pid)

        self._workers[fault_id] = workers

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Terminate {cores} CPU stress workers",
            domain="cpu",
            fault_type="stress",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        workers = self._workers.pop(fault_id, [])
        if not workers:
            logger.info("[%s] No CPU stress workers to terminate (already cleaned up)", fault_id)
            return

        for p in workers:
            if p.is_alive():
                logger.debug("Terminating CPU worker PID %s", p.pid)
                p.terminate()
        for p in workers:
            p.join(timeout=5)
            if p.is_alive():
                logger.warning("Force-killing CPU worker PID %s", p.pid)
                p.kill()
                p.join(timeout=2)

        logger.info("[%s] Terminated %d CPU stress workers", fault_id, len(workers))

    def status(self, fault_id: str) -> Phase:
        workers = self._workers.get(fault_id, [])
        if not workers:
            return Phase.COMPLETED
        if any(p.is_alive() for p in workers):
            return Phase.ACTIVE
        return Phase.COMPLETED


class ThrottleFault(FaultType):
    """Throttle CPU via cgroup cpu.cfs_quota_us / cpu.cfs_period_us."""

    _cgroup_paths: dict[str, str]

    def __init__(self) -> None:
        self._cgroup_paths = {}

    @property
    def name(self) -> str:
        return "throttle"

    @property
    def description(self) -> str:
        return "Throttle CPU usage via cgroup cpu quota"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="percent",
                type=int,
                required=True,
                unit="%",
                help="CPU quota as a percentage of one core (e.g. 50 = half a core)",
            ),
            ParameterSpec(
                name="pid",
                type=int,
                required=False,
                default=None,
                help="PID of the process to throttle (adds to cgroup)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        percent = config.params.get("percent")
        if percent is None:
            errors.append("'percent' is required")
        elif not isinstance(percent, int) or not (1 <= percent <= 100):
            errors.append("'percent' must be an integer between 1 and 100")

        pid = config.params.get("pid")
        if pid is not None and (not isinstance(pid, int) or pid < 1):
            errors.append("'pid' must be a positive integer")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        percent: int = config.params["percent"]
        pid: int | None = config.params.get("pid")
        cgroup_name = f"asuran_throttle_{fault_id[:8]}"
        cgroup_path = f"/sys/fs/cgroup/cpu/{cgroup_name}"
        period_us = 100_000
        quota_us = int(period_us * percent / 100)

        logger.info(
            "[%s] Injecting CPU throttle: %d%% (quota=%d period=%d)",
            fault_id, percent, quota_us, period_us,
        )

        if config.dry_run or context.dry_run:
            logger.info(
                "[DRY RUN] Would create cgroup %s with quota %d/%d",
                cgroup_path, quota_us, period_us,
            )
            self._cgroup_paths[fault_id] = cgroup_path
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Remove cgroup {cgroup_name}",
                domain="cpu",
                fault_type="throttle",
            )
            return fault_id

        # Create cgroup directory
        context.run_cmd(["mkdir", "-p", cgroup_path])
        # Set period and quota
        context.run_cmd(["sh", "-c", f"echo {period_us} > {cgroup_path}/cpu.cfs_period_us"])
        context.run_cmd(["sh", "-c", f"echo {quota_us} > {cgroup_path}/cpu.cfs_quota_us"])

        # Optionally move a process into the cgroup
        if pid is not None:
            context.run_cmd(["sh", "-c", f"echo {pid} > {cgroup_path}/cgroup.procs"])
            logger.info("[%s] Moved PID %d into cgroup %s", fault_id, pid, cgroup_name)

        self._cgroup_paths[fault_id] = cgroup_path

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Remove cgroup {cgroup_name}",
            domain="cpu",
            fault_type="throttle",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        cgroup_path = self._cgroup_paths.pop(fault_id, None)
        if cgroup_path is None:
            logger.info("[%s] No cgroup to remove (already cleaned up)", fault_id)
            return

        # Remove the cgroup directory (kernel removes it when empty)
        context.run_cmd(["rmdir", cgroup_path], check=False)
        logger.info("[%s] Removed cgroup %s", fault_id, cgroup_path)

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._cgroup_paths:
            return Phase.ACTIVE
        return Phase.COMPLETED


class CpuDomain(ChaosDomain):
    """CPU chaos domain."""

    @property
    def name(self) -> str:
        return "cpu"

    @property
    def description(self) -> str:
        return "CPU chaos: stress, throttle, affinity"

    def fault_types(self) -> list[FaultType]:
        return [StressFault(), ThrottleFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_ADMIN"]

    def required_tools(self) -> list[str]:
        return []


def create_domain() -> ChaosDomain:
    """Entry point for the plugin loader."""
    return CpuDomain()
