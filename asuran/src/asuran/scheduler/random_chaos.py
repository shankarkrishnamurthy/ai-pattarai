"""Random chaos mode -- weighted random fault selection."""
from __future__ import annotations

import logging
import random
import threading
import time
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from asuran.core.types import FaultConfig, Severity

if TYPE_CHECKING:
    from asuran.core.context import ExecutionContext
    from asuran.core.executor import FaultExecutor
    from asuran.core.plugin import FaultType
    from asuran.core.registry import PluginRegistry

logger = logging.getLogger(__name__)

# Map severity levels to integer values for comparison.
_SEVERITY_ORDER: dict[Severity, int] = {
    Severity.LOW: 1,
    Severity.MEDIUM: 2,
    Severity.HIGH: 3,
    Severity.CRITICAL: 4,
}


@dataclass
class RandomChaosConfig:
    """Configuration for random chaos mode."""

    domains: list[str] = field(default_factory=list)
    """Which fault domains to pick from (e.g. ``["network", "cpu"]``)."""

    interval_range: tuple[float, float] = (60.0, 300.0)
    """Min/max seconds between successive fault injections."""

    severity_max: Severity = Severity.MEDIUM
    """Only pick faults at or below this severity level."""

    duration_range: tuple[float, float] = (10.0, 60.0)
    """Min/max duration (seconds) for each injected fault."""


class RandomChaos:
    """Periodically pick and inject random faults from configured domains.

    Parameters
    ----------
    registry:
        The ``PluginRegistry`` containing discovered chaos domains.
    executor:
        A ``FaultExecutor`` used to inject/rollback faults.
    context:
        The ``ExecutionContext`` passed through to faults.
    config:
        A ``RandomChaosConfig`` controlling which faults may be selected.
    """

    def __init__(
        self,
        registry: PluginRegistry,
        executor: FaultExecutor,
        context: ExecutionContext,
        config: RandomChaosConfig,
    ) -> None:
        self._registry = registry
        self._executor = executor
        self._context = context
        self._config = config
        self._running = False
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start the background random-chaos thread."""
        if self._running:
            logger.warning("RandomChaos is already running")
            return

        self._stop_event.clear()
        self._running = True
        self._thread = threading.Thread(
            target=self._run_loop,
            name="asuran-random-chaos",
            daemon=True,
        )
        self._thread.start()
        logger.info("RandomChaos started (domains=%s)", self._config.domains)

    def stop(self) -> None:
        """Signal the background thread to stop and wait for it to finish."""
        if not self._running:
            return

        self._stop_event.set()
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=max(self._config.interval_range) + 5)
            self._thread = None
        logger.info("RandomChaos stopped")

    # ------------------------------------------------------------------
    # Internal loop
    # ------------------------------------------------------------------

    def _run_loop(self) -> None:
        """Main loop: sleep for a random interval, then pick and inject a fault."""
        while not self._stop_event.is_set():
            # Sleep a random interval before the next fault.
            delay = random.uniform(*self._config.interval_range)
            if self._stop_event.wait(timeout=delay):
                break  # stop was requested during the wait

            try:
                pick = self._pick_fault()
                if pick is None:
                    logger.warning("No eligible fault found; skipping this cycle")
                    continue

                fault_type, fault_config = pick
                logger.info(
                    "RandomChaos injecting fault: %s (duration=%.1fs)",
                    fault_type.name,
                    fault_config.duration or 0.0,
                )
                result = self._executor.execute(fault_type, fault_config)
                logger.info(
                    "RandomChaos fault %s result: success=%s, phase=%s",
                    result.fault_id,
                    result.success,
                    result.phase,
                )
            except Exception:
                logger.exception("RandomChaos encountered an error during injection")

    # ------------------------------------------------------------------
    # Fault selection
    # ------------------------------------------------------------------

    def _pick_fault(self) -> tuple[FaultType, FaultConfig] | None:
        """Randomly select a domain and fault type, returning a ``(FaultType, FaultConfig)``
        tuple, or ``None`` if no eligible faults exist.

        Only fault types whose severity is at or below ``config.severity_max``
        are considered.
        """
        max_sev = _SEVERITY_ORDER.get(self._config.severity_max, 0)

        # Collect eligible fault types across the configured domains.
        eligible: list[FaultType] = []
        domains_to_search = self._config.domains or self._registry.domain_names()

        for domain_name in domains_to_search:
            domain = self._registry.get_domain(domain_name)
            if domain is None:
                logger.debug("Domain %r not found in registry, skipping", domain_name)
                continue
            for ft in domain.fault_types():
                ft_sev = _SEVERITY_ORDER.get(ft.severity, 0)
                if ft_sev <= max_sev:
                    eligible.append(ft)

        if not eligible:
            return None

        chosen = random.choice(eligible)

        # Build a FaultConfig with a random duration.
        duration = random.uniform(*self._config.duration_range)
        config = FaultConfig(
            params={},
            duration=duration,
        )

        return chosen, config
