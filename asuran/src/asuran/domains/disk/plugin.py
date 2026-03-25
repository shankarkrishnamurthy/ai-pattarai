"""Disk chaos domain — fill, I/O latency, IOPS throttling."""
from __future__ import annotations

import logging
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


class FillFault(FaultType):
    """Fill disk space by creating a large file via fallocate or dd."""

    _fill_files: dict[str, str]  # fault_id -> file path

    def __init__(self) -> None:
        self._fill_files = {}

    @property
    def name(self) -> str:
        return "fill"

    @property
    def description(self) -> str:
        return "Fill disk space by allocating a large file"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="path",
                type=str,
                required=True,
                default="/tmp",
                positional=True,
                help="Directory where the fill file will be created",
            ),
            ParameterSpec(
                name="size",
                type=str,
                required=True,
                help="Size of fill file (e.g. '1G', '512M', '100K')",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        path = config.params.get("path", "/tmp")
        if not isinstance(path, str) or not path:
            errors.append("'path' must be a non-empty string")
        elif not os.path.isdir(path):
            errors.append(f"'path' directory does not exist: {path}")

        size = config.params.get("size")
        if size is None:
            errors.append("'size' is required")
        elif not isinstance(size, str):
            errors.append("'size' must be a string (e.g. '1G')")
        else:
            try:
                parsed = _parse_size(size)
                if parsed <= 0:
                    errors.append("'size' must be positive")
            except (ValueError, IndexError):
                errors.append("'size' must be a valid size string (e.g. '1G', '512M')")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        path: str = config.params.get("path", "/tmp")
        size_str: str = config.params["size"]
        alloc_bytes = _parse_size(size_str)
        fill_file = os.path.join(path, f"asuran_fill_{fault_id}")

        logger.info(
            "[%s] Injecting disk fill: %s (%d bytes) at %s",
            fault_id, size_str, alloc_bytes, fill_file,
        )

        if config.dry_run or context.dry_run:
            logger.info("[DRY RUN] Would create fill file %s (%s)", fill_file, size_str)
            self._fill_files[fault_id] = fill_file
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Delete fill file {fill_file}",
                domain="disk",
                fault_type="fill",
            )
            return fault_id

        # Try fallocate first, fall back to dd
        result = context.run_cmd(
            ["fallocate", "-l", str(alloc_bytes), fill_file],
            check=False,
        )
        if result.returncode != 0:
            logger.info("[%s] fallocate failed, falling back to dd", fault_id)
            block_size = 1024 * 1024  # 1M
            count = max(1, alloc_bytes // block_size)
            context.run_cmd([
                "dd", "if=/dev/zero", f"of={fill_file}",
                f"bs={block_size}", f"count={count}",
            ])

        self._fill_files[fault_id] = fill_file
        logger.info("[%s] Created fill file %s", fault_id, fill_file)

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Delete fill file {fill_file}",
            domain="disk",
            fault_type="fill",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        fill_file = self._fill_files.pop(fault_id, None)
        if fill_file is None:
            logger.info("[%s] No fill file to remove (already cleaned up)", fault_id)
            return

        try:
            if os.path.exists(fill_file):
                os.unlink(fill_file)
                logger.info("[%s] Deleted fill file %s", fault_id, fill_file)
            else:
                logger.info("[%s] Fill file already gone: %s", fault_id, fill_file)
        except OSError as exc:
            logger.error("[%s] Failed to delete fill file %s: %s", fault_id, fill_file, exc)

    def status(self, fault_id: str) -> Phase:
        fill_file = self._fill_files.get(fault_id)
        if fill_file and os.path.exists(fill_file):
            return Phase.ACTIVE
        return Phase.COMPLETED


class IoLatencyFault(FaultType):
    """Inject I/O latency via dm-delay (placeholder — requires root + device-mapper setup)."""

    _active: dict[str, dict[str, Any]]

    def __init__(self) -> None:
        self._active = {}

    @property
    def name(self) -> str:
        return "latency"

    @property
    def description(self) -> str:
        return "Inject I/O latency on a block device (placeholder for dm-delay)"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="device",
                type=str,
                required=True,
                help="Block device path (e.g. '/dev/sda')",
            ),
            ParameterSpec(
                name="delay",
                type=int,
                required=True,
                unit="ms",
                help="Latency to add in milliseconds",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        device = config.params.get("device")
        if device is None:
            errors.append("'device' is required")
        elif not isinstance(device, str) or not device:
            errors.append("'device' must be a non-empty string")

        delay = config.params.get("delay")
        if delay is None:
            errors.append("'delay' is required")
        elif not isinstance(delay, int) or delay < 1:
            errors.append("'delay' must be a positive integer (milliseconds)")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        device: str = config.params["device"]
        delay: int = config.params["delay"]

        logger.info(
            "[%s] I/O latency injection: device=%s delay=%dms",
            fault_id, device, delay,
        )

        # This is a placeholder — dm-delay requires root, device-mapper setup,
        # and careful teardown. Log what would be done.
        logger.warning(
            "[%s] dm-delay injection is a placeholder. "
            "Full implementation requires: dmsetup create asuran_delay_%s "
            "--table '0 <sectors> delay %s 0 %d'",
            fault_id, fault_id[:8], device, delay,
        )

        if config.dry_run or context.dry_run:
            logger.info(
                "[DRY RUN] Would inject %dms latency on %s via dm-delay",
                delay, device,
            )

        self._active[fault_id] = {"device": device, "delay": delay}

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Remove I/O latency on {device}",
            domain="disk",
            fault_type="latency",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        entry = self._active.pop(fault_id, None)
        if entry is None:
            logger.info("[%s] No I/O latency fault to roll back", fault_id)
            return

        logger.info(
            "[%s] Would remove dm-delay device (placeholder rollback): device=%s",
            fault_id, entry["device"],
        )

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._active:
            return Phase.ACTIVE
        return Phase.COMPLETED


class IopsFault(FaultType):
    """Throttle IOPS via cgroup blkio controller."""

    _cgroup_paths: dict[str, str]

    def __init__(self) -> None:
        self._cgroup_paths = {}

    @property
    def name(self) -> str:
        return "iops"

    @property
    def description(self) -> str:
        return "Throttle IOPS on a block device via cgroup blkio"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="device",
                type=str,
                required=True,
                help="Block device path (e.g. '/dev/sda')",
            ),
            ParameterSpec(
                name="limit",
                type=int,
                required=True,
                help="Maximum IOPS allowed",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        device = config.params.get("device")
        if device is None:
            errors.append("'device' is required")
        elif not isinstance(device, str) or not device:
            errors.append("'device' must be a non-empty string")

        limit = config.params.get("limit")
        if limit is None:
            errors.append("'limit' is required")
        elif not isinstance(limit, int) or limit < 1:
            errors.append("'limit' must be a positive integer")

        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        device: str = config.params["device"]
        limit: int = config.params["limit"]
        cgroup_name = f"asuran_iops_{fault_id[:8]}"
        cgroup_path = f"/sys/fs/cgroup/blkio/{cgroup_name}"

        logger.info(
            "[%s] Injecting IOPS throttle: device=%s limit=%d",
            fault_id, device, limit,
        )

        if config.dry_run or context.dry_run:
            logger.info(
                "[DRY RUN] Would create blkio cgroup %s with IOPS limit %d on %s",
                cgroup_path, limit, device,
            )
            self._cgroup_paths[fault_id] = cgroup_path
            context.rollback_manager.push(
                fault_id,
                lambda fid=fault_id: self.rollback(fid, context),
                f"Remove blkio cgroup {cgroup_name}",
                domain="disk",
                fault_type="iops",
            )
            return fault_id

        # Resolve device major:minor number
        result = context.run_cmd(["stat", "-c", "%t:%T", device])
        major_minor = result.stdout.strip()
        # Convert hex to decimal major:minor
        parts = major_minor.split(":")
        major = int(parts[0], 16)
        minor = int(parts[1], 16)

        # Create cgroup and set IOPS limit
        context.run_cmd(["mkdir", "-p", cgroup_path])

        # Set read IOPS limit
        throttle_read = f"{major}:{minor} {limit}"
        context.run_cmd([
            "sh", "-c",
            f"echo '{throttle_read}' > {cgroup_path}/blkio.throttle.read_iops_device",
        ])

        # Set write IOPS limit
        throttle_write = f"{major}:{minor} {limit}"
        context.run_cmd([
            "sh", "-c",
            f"echo '{throttle_write}' > {cgroup_path}/blkio.throttle.write_iops_device",
        ])

        self._cgroup_paths[fault_id] = cgroup_path
        logger.info("[%s] Created blkio cgroup %s (IOPS limit: %d)", fault_id, cgroup_name, limit)

        context.rollback_manager.push(
            fault_id,
            lambda fid=fault_id: self.rollback(fid, context),
            f"Remove blkio cgroup {cgroup_name}",
            domain="disk",
            fault_type="iops",
        )

        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        cgroup_path = self._cgroup_paths.pop(fault_id, None)
        if cgroup_path is None:
            logger.info("[%s] No blkio cgroup to remove (already cleaned up)", fault_id)
            return

        context.run_cmd(["rmdir", cgroup_path], check=False)
        logger.info("[%s] Removed blkio cgroup %s", fault_id, cgroup_path)

    def status(self, fault_id: str) -> Phase:
        if fault_id in self._cgroup_paths:
            return Phase.ACTIVE
        return Phase.COMPLETED


class DiskDomain(ChaosDomain):
    """Disk chaos domain."""

    @property
    def name(self) -> str:
        return "disk"

    @property
    def description(self) -> str:
        return "Disk chaos: fill, latency, I/O errors"

    def fault_types(self) -> list[FaultType]:
        return [FillFault(), IoLatencyFault(), IopsFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_ADMIN"]

    def required_tools(self) -> list[str]:
        return ["fallocate", "dd"]


def create_domain() -> ChaosDomain:
    """Entry point for the plugin loader."""
    return DiskDomain()
