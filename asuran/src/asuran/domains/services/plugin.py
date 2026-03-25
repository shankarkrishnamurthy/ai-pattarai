"""Service chaos domain: systemd service stop and mask."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "svc"


class StopServiceFault(FaultType):
    """Stop a systemd service."""

    @property
    def name(self) -> str:
        return "stop"

    @property
    def description(self) -> str:
        return "Stop a systemd service unit"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="unit", type=str, required=True, help="Systemd unit name (e.g. nginx.service)"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "unit" not in config.params:
            errors.append("Missing required parameter: unit")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        unit = config.params["unit"]

        if config.dry_run:
            logger.info("[DRY RUN] Would stop service %s", unit)
            return fault_id

        context.run_cmd(["systemctl", "stop", unit])
        logger.info("Stopped service %s", unit)

        def rollback_fn() -> None:
            context.run_cmd(["systemctl", "start", unit])
            logger.info("Started service %s", unit)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restart service {unit}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class MaskServiceFault(FaultType):
    """Mask a systemd service to prevent it from starting."""

    @property
    def name(self) -> str:
        return "mask"

    @property
    def description(self) -> str:
        return "Mask a systemd service unit to prevent starting"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="unit", type=str, required=True, help="Systemd unit name to mask"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "unit" not in config.params:
            errors.append("Missing required parameter: unit")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        unit = config.params["unit"]

        if config.dry_run:
            logger.info("[DRY RUN] Would mask service %s", unit)
            return fault_id

        context.run_cmd(["systemctl", "mask", unit])
        logger.info("Masked service %s", unit)

        def rollback_fn() -> None:
            context.run_cmd(["systemctl", "unmask", unit])
            logger.info("Unmasked service %s", unit)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Unmask service {unit}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class ServicesDomain(ChaosDomain):
    """Service chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Service chaos: systemd, cron"

    def fault_types(self) -> list[FaultType]:
        return [StopServiceFault(), MaskServiceFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_ADMIN"]

    def required_tools(self) -> list[str]:
        return ["systemctl"]


def create_domain() -> ChaosDomain:
    return ServicesDomain()
