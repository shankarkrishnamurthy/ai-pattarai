"""Time chaos domain: clock skew and NTP disruption."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "time"


class SkewFault(FaultType):
    """Skew the system clock by a given offset."""

    @property
    def name(self) -> str:
        return "skew"

    @property
    def description(self) -> str:
        return "Offset the system clock using date -s"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="offset", type=int, required=True, help="Clock offset in seconds", unit="s"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "offset" not in config.params:
            errors.append("Missing required parameter: offset")
        elif not isinstance(config.params["offset"], int):
            errors.append("Parameter 'offset' must be an integer")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        offset = config.params["offset"]

        if config.dry_run:
            logger.info("[DRY RUN] Would skew clock by %+d seconds", offset)
            return fault_id

        sign = "+" if offset >= 0 else "-"
        abs_offset = abs(offset)
        context.run_cmd(["date", "-s", f"{sign}{abs_offset} seconds"])
        logger.info("Skewed system clock by %+d seconds", offset)

        def rollback_fn() -> None:
            reverse_sign = "-" if offset >= 0 else "+"
            context.run_cmd(["date", "-s", f"{reverse_sign}{abs_offset} seconds"])
            logger.info("Reversed clock skew of %+d seconds", offset)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Reverse clock skew of {offset}s",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class NtpFault(FaultType):
    """Stop the NTP service to prevent clock synchronisation."""

    @property
    def name(self) -> str:
        return "ntp"

    @property
    def description(self) -> str:
        return "Stop chronyd/ntpd to prevent clock sync"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return []

    def validate(self, config: FaultConfig) -> list[str]:
        return []

    def _detect_service(self, context: Any) -> str:
        """Detect whether chronyd or ntpd is the active NTP service."""
        for svc in ("chronyd", "ntpd"):
            result = context.run_cmd(["systemctl", "is-active", svc], check=False)
            if result.stdout.strip() == "active":
                return svc
        return "chronyd"  # default fallback

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]

        if config.dry_run:
            logger.info("[DRY RUN] Would stop NTP service")
            return fault_id

        service = self._detect_service(context)
        context.run_cmd(["systemctl", "stop", service])
        logger.info("Stopped NTP service: %s", service)

        def rollback_fn() -> None:
            context.run_cmd(["systemctl", "start", service])
            logger.info("Restarted NTP service: %s", service)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restart NTP service {service}",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class TimeDomain(ChaosDomain):
    """Time chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Time chaos: clock skew, NTP, timezone"

    def fault_types(self) -> list[FaultType]:
        return [SkewFault(), NtpFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_TIME"]

    def required_tools(self) -> list[str]:
        return ["date", "systemctl"]


def create_domain() -> ChaosDomain:
    return TimeDomain()
