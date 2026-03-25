"""Filesystem chaos domain: permission changes and inode exhaustion."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "fs"


class PermissionsFault(FaultType):
    """Change file or directory permissions."""

    @property
    def name(self) -> str:
        return "permissions"

    @property
    def description(self) -> str:
        return "Change filesystem permissions via chmod"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="path", type=str, required=True, help="Target file or directory path"),
            ParameterSpec(name="mode", type=str, required=True, help="New permission mode (e.g. 000, 777)"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "path" not in config.params:
            errors.append("Missing required parameter: path")
        if "mode" not in config.params:
            errors.append("Missing required parameter: mode")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        path = config.params["path"]
        new_mode = config.params["mode"]

        if config.dry_run:
            logger.info("[DRY RUN] Would chmod %s to %s", path, new_mode)
            return fault_id

        # Save original mode
        result = context.run_cmd(["stat", "-c", "%a", path])
        original_mode = result.stdout.strip()

        context.run_cmd(["chmod", new_mode, path])
        logger.info("Changed permissions on %s from %s to %s", path, original_mode, new_mode)

        def rollback_fn() -> None:
            context.run_cmd(["chmod", original_mode, path])
            logger.info("Restored permissions on %s to %s", path, original_mode)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restore {path} permissions to {original_mode}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class InodesFault(FaultType):
    """Exhaust inodes by creating thousands of small files."""

    @property
    def name(self) -> str:
        return "inodes"

    @property
    def description(self) -> str:
        return "Exhaust inodes by creating many small files in a directory"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="path", type=str, required=True, help="Directory to fill with small files"),
            ParameterSpec(name="count", type=int, required=False, default=10000, help="Number of files to create"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "path" not in config.params:
            errors.append("Missing required parameter: path")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        path = config.params["path"]
        count = config.params.get("count", 10000)
        subdir = f"{path}/.asuran_inodes_{fault_id}"

        if config.dry_run:
            logger.info("[DRY RUN] Would create %d files in %s", count, subdir)
            return fault_id

        context.run_cmd(["mkdir", "-p", subdir])
        # Create files in batches using shell expansion
        context.run_cmd([
            "bash", "-c",
            f"for i in $(seq 1 {count}); do touch {subdir}/f_$i; done",
        ], timeout=120.0)
        logger.info("Created %d inode-consuming files in %s", count, subdir)

        def rollback_fn() -> None:
            context.run_cmd(["rm", "-rf", subdir])
            logger.info("Removed inode chaos directory %s", subdir)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Remove {count} chaos files from {subdir}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class FilesystemDomain(ChaosDomain):
    """Filesystem chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Filesystem chaos: permissions, inodes"

    def fault_types(self) -> list[FaultType]:
        return [PermissionsFault(), InodesFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_DAC_OVERRIDE"]

    def required_tools(self) -> list[str]:
        return ["chmod", "stat"]


def create_domain() -> ChaosDomain:
    return FilesystemDomain()
