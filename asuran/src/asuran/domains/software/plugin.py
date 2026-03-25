"""Software chaos domain: LD_PRELOAD injection."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "sw"


class PreloadFault(FaultType):
    """Inject a shared library via LD_PRELOAD for a target process (placeholder)."""

    @property
    def name(self) -> str:
        return "preload"

    @property
    def description(self) -> str:
        return "Set LD_PRELOAD environment for a target process (placeholder)"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="library", type=str, required=True, help="Path to shared library to preload"),
            ParameterSpec(name="pid", type=int, required=False, help="Target process ID (informational)"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "library" not in config.params:
            errors.append("Missing required parameter: library")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        library = config.params["library"]
        pid = config.params.get("pid")

        if config.dry_run:
            logger.info("[DRY RUN] Would set LD_PRELOAD=%s for pid=%s", library, pid)
            return fault_id

        # Placeholder: LD_PRELOAD injection into a running process requires
        # ptrace or /proc/<pid>/environ manipulation, which is highly invasive.
        # This logs the intent and registers a rollback stub.
        logger.warning(
            "LD_PRELOAD fault is a placeholder. Would inject library=%s for pid=%s. "
            "Actual injection requires ptrace or process restart with modified env.",
            library, pid,
        )

        def rollback_fn() -> None:
            logger.info(
                "Rollback for LD_PRELOAD fault %s: no-op (placeholder). "
                "Manual cleanup may be required.", fault_id,
            )

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Undo LD_PRELOAD of {library} (placeholder)",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class SoftwareDomain(ChaosDomain):
    """Software chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Software chaos: LD_PRELOAD, dependency removal"

    def fault_types(self) -> list[FaultType]:
        return [PreloadFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_SYS_PTRACE"]

    def required_tools(self) -> list[str]:
        return []


def create_domain() -> ChaosDomain:
    return SoftwareDomain()
