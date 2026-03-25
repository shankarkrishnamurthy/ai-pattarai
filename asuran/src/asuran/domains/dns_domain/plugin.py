"""DNS chaos domain: resolution failure and slow DNS."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "dns"


class DnsFailFault(FaultType):
    """Break DNS resolution by replacing resolv.conf."""

    @property
    def name(self) -> str:
        return "fail"

    @property
    def description(self) -> str:
        return "Break DNS resolution by pointing resolv.conf to localhost"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return []

    def validate(self, config: FaultConfig) -> list[str]:
        return []

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        resolv = "/etc/resolv.conf"
        backup = f"{resolv}.asuran_bak_{fault_id}"

        if config.dry_run:
            logger.info("[DRY RUN] Would replace %s with broken nameserver", resolv)
            return fault_id

        # Back up current resolv.conf
        context.run_cmd(["cp", "-a", resolv, backup])
        # Write broken DNS config
        context.run_cmd(["bash", "-c", f'echo "nameserver 127.0.0.1" > {resolv}'])
        logger.info("Replaced %s with broken nameserver (backup at %s)", resolv, backup)

        def rollback_fn() -> None:
            context.run_cmd(["mv", "-f", backup, resolv])
            logger.info("Restored %s from backup", resolv)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restore {resolv} from backup",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class DnsSlowFault(FaultType):
    """Add latency to DNS traffic using tc netem."""

    @property
    def name(self) -> str:
        return "slow"

    @property
    def description(self) -> str:
        return "Add latency to DNS traffic using tc netem on loopback"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="delay", type=int, required=True, help="Delay to add in milliseconds", unit="ms"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "delay" not in config.params:
            errors.append("Missing required parameter: delay")
        elif not isinstance(config.params["delay"], int):
            errors.append("Parameter 'delay' must be an integer")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        delay = config.params["delay"]

        if config.dry_run:
            logger.info("[DRY RUN] Would add %dms delay to DNS traffic", delay)
            return fault_id

        context.run_cmd([
            "tc", "qdisc", "add", "dev", "lo",
            "root", "netem", "delay", f"{delay}ms",
        ])
        logger.info("Added %dms delay to loopback via tc netem", delay)

        def rollback_fn() -> None:
            context.run_cmd(["tc", "qdisc", "del", "dev", "lo", "root", "netem"])
            logger.info("Removed tc netem delay from loopback")

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Remove {delay}ms tc netem delay from loopback",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class DnsDomain(ChaosDomain):
    """DNS chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "DNS chaos: resolution failure, slow DNS"

    def fault_types(self) -> list[FaultType]:
        return [DnsFailFault(), DnsSlowFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_NET_ADMIN"]

    def required_tools(self) -> list[str]:
        return ["tc"]


def create_domain() -> ChaosDomain:
    return DnsDomain()
