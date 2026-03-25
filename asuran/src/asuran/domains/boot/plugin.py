"""Boot chaos domain: kernel boot parameter manipulation."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "boot"


class KparamFault(FaultType):
    """Modify kernel boot parameters in GRUB configuration (placeholder)."""

    @property
    def name(self) -> str:
        return "kparam"

    @property
    def description(self) -> str:
        return "Modify kernel boot parameters in GRUB config (requires reboot to take effect)"

    @property
    def severity(self) -> Severity:
        return Severity.CRITICAL

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="param", type=str, required=True, help="Kernel parameter name"),
            ParameterSpec(name="value", type=str, required=True, help="Kernel parameter value"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "param" not in config.params:
            errors.append("Missing required parameter: param")
        if "value" not in config.params:
            errors.append("Missing required parameter: value")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        param = config.params["param"]
        value = config.params["value"]
        grub_path = "/etc/default/grub"
        backup_path = f"{grub_path}.asuran_bak_{fault_id}"

        if config.dry_run:
            logger.info("[DRY RUN] Would modify GRUB param %s=%s", param, value)
            return fault_id

        # Back up GRUB config
        context.run_cmd(["cp", "-a", grub_path, backup_path])
        logger.warning(
            "Boot parameter %s=%s staged in GRUB config backup at %s. "
            "This is a placeholder — actual GRUB modification is intentionally "
            "not applied automatically. Manual review required before grub-mkconfig.",
            param, value, backup_path,
        )

        def rollback_fn() -> None:
            context.run_cmd(["mv", "-f", backup_path, grub_path])
            logger.info("Restored GRUB config from backup %s", backup_path)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restore GRUB config (undo kparam {param}={value})",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class BootDomain(ChaosDomain):
    """Boot chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Boot chaos: kernel params"

    def fault_types(self) -> list[FaultType]:
        return [KparamFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_ADMIN"]

    def required_tools(self) -> list[str]:
        return ["cp"]


def create_domain() -> ChaosDomain:
    return BootDomain()
