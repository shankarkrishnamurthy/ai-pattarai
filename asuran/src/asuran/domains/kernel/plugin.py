"""Kernel chaos domain: sysctl parameter manipulation and kernel module unloading."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "kern"


class SysctlFault(FaultType):
    """Modify a sysctl kernel parameter."""

    @property
    def name(self) -> str:
        return "sysctl"

    @property
    def description(self) -> str:
        return "Override a sysctl kernel parameter and restore on rollback"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="key", type=str, required=True, help="Sysctl key path (e.g. net.ipv4.ip_forward)"),
            ParameterSpec(name="value", type=str, required=True, help="Value to set"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "key" not in config.params:
            errors.append("Missing required parameter: key")
        if "value" not in config.params:
            errors.append("Missing required parameter: value")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        key = config.params["key"]
        new_value = config.params["value"]
        proc_path = f"/proc/sys/{key.replace('.', '/')}"

        if config.dry_run:
            logger.info("[DRY RUN] Would set %s = %s", key, new_value)
            return fault_id

        # Read old value
        result = context.run_cmd(["cat", proc_path])
        old_value = result.stdout.strip()

        # Write new value
        context.run_cmd(["bash", "-c", f"echo {new_value} > {proc_path}"])
        logger.info("Set sysctl %s = %s (was %s)", key, new_value, old_value)

        def rollback_fn() -> None:
            context.run_cmd(["bash", "-c", f"echo {old_value} > {proc_path}"])
            logger.info("Restored sysctl %s = %s", key, old_value)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restore sysctl {key} to {old_value}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class ModuleFault(FaultType):
    """Unload a kernel module."""

    @property
    def name(self) -> str:
        return "module"

    @property
    def description(self) -> str:
        return "Unload a kernel module via rmmod"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="name", type=str, required=True, help="Kernel module name to unload"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "name" not in config.params:
            errors.append("Missing required parameter: name")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        mod_name = config.params["name"]

        if config.dry_run:
            logger.info("[DRY RUN] Would unload module %s", mod_name)
            return fault_id

        context.run_cmd(["rmmod", mod_name])
        logger.info("Unloaded kernel module %s", mod_name)

        def rollback_fn() -> None:
            context.run_cmd(["modprobe", mod_name])
            logger.info("Reloaded kernel module %s", mod_name)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Reload kernel module {mod_name}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class KernelDomain(ChaosDomain):
    """Kernel chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Kernel chaos: sysctl, modules"

    def fault_types(self) -> list[FaultType]:
        return [SysctlFault(), ModuleFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_ADMIN"]

    def required_tools(self) -> list[str]:
        return ["rmmod", "modprobe"]


def create_domain() -> ChaosDomain:
    return KernelDomain()
