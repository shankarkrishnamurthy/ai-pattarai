"""Hardware chaos domain: NIC manipulation."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "hw"


class NicDownFault(FaultType):
    """Bring a network interface down."""

    @property
    def name(self) -> str:
        return "nic-down"

    @property
    def description(self) -> str:
        return "Bring a network interface down via ip link set"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="interface", type=str, required=True, help="Network interface name (e.g. eth0)"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "interface" not in config.params:
            errors.append("Missing required parameter: interface")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        iface = config.params["interface"]

        if config.dry_run:
            logger.info("[DRY RUN] Would bring down interface %s", iface)
            return fault_id

        context.run_cmd(["ip", "link", "set", iface, "down"])
        logger.info("Brought down interface %s", iface)

        def rollback_fn() -> None:
            context.run_cmd(["ip", "link", "set", iface, "up"])
            logger.info("Brought up interface %s", iface)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Bring interface {iface} back up",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class HardwareDomain(ChaosDomain):
    """Hardware chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Hardware chaos: NIC, PCIe"

    def fault_types(self) -> list[FaultType]:
        return [NicDownFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_NET_ADMIN"]

    def required_tools(self) -> list[str]:
        return ["ip"]


def create_domain() -> ChaosDomain:
    return HardwareDomain()
