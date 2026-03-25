"""Sequential and parallel step orchestration."""
from __future__ import annotations

import logging
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any

from asuran.core.executor import FaultExecutor
from asuran.core.registry import PluginRegistry
from asuran.core.types import FaultConfig, FaultResult, Phase

logger = logging.getLogger(__name__)


class StepRunner:
    """Runs experiment steps sequentially or in parallel groups.

    The runner resolves fault types from the registry and delegates
    execution to the ``FaultExecutor``.
    """

    def __init__(self, registry: PluginRegistry, executor: FaultExecutor) -> None:
        self._registry = registry
        self._executor = executor

    def run_steps(
        self,
        steps: list[Any],  # list[StepConfig] — forward ref
        variables: dict[str, Any],
    ) -> list[FaultResult]:
        """Execute a list of steps, grouping parallel steps automatically.

        Steps that share a non-None ``group`` name are collected and run
        concurrently via :meth:`_run_parallel_group`.  All other steps
        run one at a time in order.

        Args:
            steps: Ordered step configurations.
            variables: Plan-level variables for argument resolution.

        Returns:
            Aggregated list of ``FaultResult`` objects.
        """
        from asuran.engine.experiment import _group_steps

        all_results: list[FaultResult] = []
        groups = _group_steps(steps)

        for group in groups:
            if len(group) > 1 or group[0].parallel:
                results = self._run_parallel_group(group, variables)
            else:
                results = [self._run_single_step(group[0], variables)]
            all_results.extend(results)

        return all_results

    def _run_parallel_group(
        self,
        steps: list[Any],  # list[StepConfig]
        variables: dict[str, Any],
    ) -> list[FaultResult]:
        """Run a group of steps concurrently using a thread pool.

        Each step is submitted to a ``ThreadPoolExecutor`` and all
        futures are collected before returning.  Exceptions inside
        individual steps are caught and returned as failed
        ``FaultResult`` objects — a single step failure does not
        prevent other steps in the group from executing.

        Args:
            steps: Steps to execute in parallel.
            variables: Plan-level variables.

        Returns:
            List of ``FaultResult`` objects in submission order.
        """
        group_name = steps[0].group or "parallel"
        logger.info(
            "Running parallel group '%s' with %d steps",
            group_name,
            len(steps),
        )

        # Map from future to original index so results stay ordered.
        results: list[FaultResult | None] = [None] * len(steps)

        with ThreadPoolExecutor(
            max_workers=len(steps),
            thread_name_prefix=f"asuran-{group_name}",
        ) as pool:
            future_to_index = {
                pool.submit(self._run_single_step, step, variables): idx
                for idx, step in enumerate(steps)
            }

            for future in as_completed(future_to_index):
                idx = future_to_index[future]
                step = steps[idx]
                try:
                    results[idx] = future.result()
                except Exception as exc:
                    logger.error(
                        "Parallel step '%s' raised an exception: %s",
                        step.name,
                        exc,
                    )
                    results[idx] = FaultResult(
                        fault_id="",
                        success=False,
                        phase=Phase.FAILED,
                        start_time=time.time(),
                        end_time=time.time(),
                        error=str(exc),
                        domain=step.domain,
                        fault_type=step.fault,
                    )

        # At this point every slot is filled (no None remains).
        return [r for r in results if r is not None]

    def _run_single_step(
        self,
        step: Any,  # StepConfig
        variables: dict[str, Any],
    ) -> FaultResult:
        """Execute one step: resolve variables, look up the fault type, run it.

        Args:
            step: The step configuration.
            variables: Plan-level variables for ``${var}`` expansion.

        Returns:
            A ``FaultResult`` representing the outcome.
        """
        from asuran.engine.experiment import _resolve_variables

        # Honour pre-step delay
        if step.delay_before > 0:
            logger.debug(
                "Delaying %.1fs before step '%s'", step.delay_before, step.name
            )
            time.sleep(step.delay_before)

        # Resolve variables in args
        resolved_args = _resolve_variables(step.args, variables)

        # Look up the fault type from the registry
        fault_type = self._registry.get_fault_type(step.domain, step.fault)
        if fault_type is None:
            logger.error(
                "Unknown fault type: %s/%s for step '%s'",
                step.domain,
                step.fault,
                step.name,
            )
            return FaultResult(
                fault_id="",
                success=False,
                phase=Phase.FAILED,
                start_time=time.time(),
                end_time=time.time(),
                error=f"Unknown fault type: {step.domain}/{step.fault}",
                domain=step.domain,
                fault_type=step.fault,
            )

        config = FaultConfig(
            params=resolved_args,
            duration=step.duration,
            dry_run=False,
            experiment_name=None,
        )

        logger.info(
            "Executing step '%s' (%s/%s)", step.name, step.domain, step.fault
        )
        return self._executor.execute(fault_type, config)
