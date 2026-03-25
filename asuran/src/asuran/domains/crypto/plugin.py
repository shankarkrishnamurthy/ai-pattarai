"""Crypto chaos domain: certificate corruption."""
from __future__ import annotations

import logging
import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase

logger = logging.getLogger(__name__)

DOMAIN_NAME = "crypto"


class CertCorruptFault(FaultType):
    """Corrupt a certificate file by appending garbage bytes."""

    @property
    def name(self) -> str:
        return "cert-corrupt"

    @property
    def description(self) -> str:
        return "Corrupt a certificate file by appending random bytes (backup preserved)"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(name="path", type=str, required=True, help="Path to the certificate file to corrupt"),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        if "path" not in config.params:
            errors.append("Missing required parameter: path")
        return errors

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())[:8]
        path = config.params["path"]
        backup_path = f"{path}.asuran_bak_{fault_id}"

        if config.dry_run:
            logger.info("[DRY RUN] Would corrupt certificate at %s", path)
            return fault_id

        # Back up original
        context.run_cmd(["cp", "-a", path, backup_path])
        logger.info("Backed up %s to %s", path, backup_path)

        # Append garbage bytes
        context.run_cmd([
            "bash", "-c",
            f"dd if=/dev/urandom bs=64 count=1 2>/dev/null >> {path}",
        ])
        logger.info("Corrupted certificate at %s", path)

        def rollback_fn() -> None:
            context.run_cmd(["mv", "-f", backup_path, path])
            logger.info("Restored certificate at %s from backup", path)

        context.rollback_manager.push(
            fault_id, rollback_fn,
            description=f"Restore certificate {path} from backup",
            domain=DOMAIN_NAME, fault_type=self.name,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class CryptoDomain(ChaosDomain):
    """Crypto chaos domain."""

    @property
    def name(self) -> str:
        return DOMAIN_NAME

    @property
    def description(self) -> str:
        return "Crypto chaos: certificates, ciphers"

    def fault_types(self) -> list[FaultType]:
        return [CertCorruptFault()]

    def required_capabilities(self) -> list[str]:
        return ["CAP_DAC_OVERRIDE"]

    def required_tools(self) -> list[str]:
        return ["cp", "dd"]


def create_domain() -> ChaosDomain:
    return CryptoDomain()
