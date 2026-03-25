"""YAML experiment plan loader and executor."""
from __future__ import annotations

import logging
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

from asuran.core.context import ExecutionContext
from asuran.core.errors import ExperimentError
from asuran.core.executor import FaultExecutor
from asuran.core.registry import PluginRegistry
from asuran.core.types import (
    ExperimentStatus,
    FaultConfig,
    FaultResult,
    OnFailure,
    Phase,
    RollbackStrategy,
)
from asuran.engine.hypothesis import SteadyStateCheck, check_steady_state
from asuran.engine.step import StepRunner

logger = logging.getLogger(__name__)

_VAR_PATTERN = re.compile(r"\$\{(\w+)\}")


@dataclass
class StepConfig:
    """Configuration for a single experiment step."""

    name: str
    domain: str
    fault: str
    args: dict[str, Any] = field(default_factory=dict)
    duration: Optional[float] = None
    parallel: bool = False
    group: Optional[str] = None
    delay_before: float = 0


@dataclass
class ExperimentPlan:
    """A complete experiment plan loaded from YAML."""

    name: str
    version: str = "1.0"
    tags: list[str] = field(default_factory=list)
    variables: dict[str, Any] = field(default_factory=dict)
    steady_state: dict[str, Any] = field(default_factory=dict)
    steps: list[StepConfig] = field(default_factory=list)
    rollback_strategy: RollbackStrategy = RollbackStrategy.END
    on_failure: OnFailure = OnFailure.ABORT


@dataclass
class ExperimentResult:
    """Outcome of an experiment run."""

    name: str
    status: ExperimentStatus
    steps_completed: int = 0
    steps_failed: int = 0
    fault_results: list[FaultResult] = field(default_factory=list)
    start_time: float = 0.0
    end_time: float = 0.0
    steady_state_pre: Optional[bool] = None
    steady_state_post: Optional[bool] = None


def _resolve_variable_in_value(value: Any, variables: dict[str, Any]) -> Any:
    """Resolve ${variable} references in a single value."""
    if isinstance(value, str):
        def _replacer(match: re.Match) -> str:
            var_name = match.group(1)
            if var_name not in variables:
                raise ExperimentError(
                    f"Undefined variable '${{{var_name}}}' in experiment plan"
                )
            return str(variables[var_name])

        resolved = _VAR_PATTERN.sub(_replacer, value)
        # If the entire string was a single variable reference, return the
        # original typed value (int, float, bool, etc.) instead of a string.
        single_match = _VAR_PATTERN.fullmatch(value)
        if single_match:
            return variables[single_match.group(1)]
        return resolved
    if isinstance(value, list):
        return [_resolve_variable_in_value(item, variables) for item in value]
    if isinstance(value, dict):
        return {
            k: _resolve_variable_in_value(v, variables) for k, v in value.items()
        }
    return value


def _resolve_variables(args: dict[str, Any], variables: dict[str, Any]) -> dict[str, Any]:
    """Resolve all ${variable} references in an args dictionary."""
    return {
        key: _resolve_variable_in_value(val, variables)
        for key, val in args.items()
    }


def _parse_steady_state_checks(raw: dict[str, Any]) -> list[SteadyStateCheck]:
    """Parse steady-state check definitions from the plan."""
    checks: list[SteadyStateCheck] = []
    for item in raw.get("checks", []):
        checks.append(
            SteadyStateCheck(
                name=item.get("name", "unnamed"),
                type=item.get("type", "command"),
                target=item.get("target", ""),
                expect=item.get("expect", {}),
            )
        )
    return checks


def load_plan(path: str) -> ExperimentPlan:
    """Load an experiment plan from a YAML file.

    Args:
        path: Filesystem path to the YAML plan file.

    Returns:
        A fully parsed ExperimentPlan with variable references resolved in
        step arguments.

    Raises:
        ExperimentError: If the file cannot be read or is malformed.
    """
    plan_path = Path(path)
    if not plan_path.exists():
        raise ExperimentError(f"Plan file not found: {path}")

    try:
        with open(plan_path, "r", encoding="utf-8") as fh:
            raw = yaml.safe_load(fh)
    except yaml.YAMLError as exc:
        raise ExperimentError(f"Invalid YAML in plan file: {exc}") from exc

    if not isinstance(raw, dict):
        raise ExperimentError("Plan file must contain a YAML mapping at the top level")

    name = raw.get("name")
    if not name:
        raise ExperimentError("Plan must have a 'name' field")

    variables = raw.get("variables", {})

    # Parse rollback strategy
    rollback_str = raw.get("rollback_strategy", "end")
    try:
        rollback_strategy = RollbackStrategy(rollback_str)
    except ValueError:
        raise ExperimentError(
            f"Invalid rollback_strategy '{rollback_str}'. "
            f"Valid values: {[e.value for e in RollbackStrategy]}"
        )

    # Parse on_failure strategy
    on_failure_str = raw.get("on_failure", "abort")
    try:
        on_failure = OnFailure(on_failure_str)
    except ValueError:
        raise ExperimentError(
            f"Invalid on_failure '{on_failure_str}'. "
            f"Valid values: {[e.value for e in OnFailure]}"
        )

    # Parse steps
    steps: list[StepConfig] = []
    for i, step_raw in enumerate(raw.get("steps", [])):
        if not isinstance(step_raw, dict):
            raise ExperimentError(f"Step {i} must be a mapping")

        resolved_args = _resolve_variables(
            step_raw.get("args", {}), variables
        )

        steps.append(
            StepConfig(
                name=step_raw.get("name", f"step-{i}"),
                domain=step_raw.get("domain", ""),
                fault=step_raw.get("fault", ""),
                args=resolved_args,
                duration=step_raw.get("duration"),
                parallel=step_raw.get("parallel", False),
                group=step_raw.get("group"),
                delay_before=step_raw.get("delay_before", 0),
            )
        )

    return ExperimentPlan(
        name=name,
        version=raw.get("version", "1.0"),
        tags=raw.get("tags", []),
        variables=variables,
        steady_state=raw.get("steady_state", {}),
        steps=steps,
        rollback_strategy=rollback_strategy,
        on_failure=on_failure,
    )


class ExperimentEngine:
    """Orchestrates the execution of an experiment plan.

    Coordinates steady-state checks, step execution, failure handling,
    and rollback according to the plan's configured strategies.
    """

    def __init__(
        self,
        registry: PluginRegistry,
        executor: FaultExecutor,
        context: ExecutionContext,
    ) -> None:
        self._registry = registry
        self._executor = executor
        self._context = context
        self._step_runner = StepRunner(registry, executor)

    def run(self, plan: ExperimentPlan) -> ExperimentResult:
        """Execute a full experiment plan.

        Phases:
            1. Pre-experiment steady-state validation
            2. Sequential / parallel step execution
            3. Post-experiment steady-state validation
            4. Rollback (per strategy)

        Args:
            plan: The experiment plan to execute.

        Returns:
            ExperimentResult summarising the run.
        """
        result = ExperimentResult(
            name=plan.name,
            status=ExperimentStatus.RUNNING,
            start_time=time.time(),
        )

        logger.info("Starting experiment: %s (v%s)", plan.name, plan.version)

        # -- 1. Pre steady-state checks --
        ss_checks = _parse_steady_state_checks(plan.steady_state)
        if ss_checks:
            logger.info("Running %d pre steady-state checks", len(ss_checks))
            passed, details = check_steady_state(ss_checks, self._context)
            result.steady_state_pre = passed
            if not passed:
                logger.error("Pre steady-state checks failed: %s", details)
                result.status = ExperimentStatus.FAILED
                result.end_time = time.time()
                return result
            logger.info("Pre steady-state checks passed")

        # -- 2. Execute steps --
        try:
            fault_results = self._execute_steps(plan)
            result.fault_results = fault_results

            for fr in fault_results:
                if fr.success:
                    result.steps_completed += 1
                else:
                    result.steps_failed += 1

        except _AbortExperiment as exc:
            logger.error("Experiment aborted: %s", exc)
            result.fault_results = list(exc.results)
            for fr in exc.results:
                if fr.success:
                    result.steps_completed += 1
                else:
                    result.steps_failed += 1
            result.status = ExperimentStatus.ABORTED
            self._handle_rollback(plan.rollback_strategy)
            result.end_time = time.time()
            return result

        # -- 3. Post steady-state checks --
        if ss_checks:
            logger.info("Running %d post steady-state checks", len(ss_checks))
            passed, details = check_steady_state(ss_checks, self._context)
            result.steady_state_post = passed
            if not passed:
                logger.warning("Post steady-state checks failed: %s", details)
        else:
            result.steady_state_post = None

        # -- 4. Rollback --
        self._handle_rollback(plan.rollback_strategy)

        # -- 5. Determine final status --
        if result.steps_failed > 0:
            result.status = ExperimentStatus.FAILED
        else:
            result.status = ExperimentStatus.COMPLETED

        result.end_time = time.time()
        duration = result.end_time - result.start_time
        logger.info(
            "Experiment '%s' finished in %.2fs — status=%s, "
            "completed=%d, failed=%d",
            plan.name,
            duration,
            result.status.value,
            result.steps_completed,
            result.steps_failed,
        )
        return result

    def _execute_steps(self, plan: ExperimentPlan) -> list[FaultResult]:
        """Run steps respecting parallel groups and on_failure strategy."""
        all_results: list[FaultResult] = []

        # Group steps: consecutive steps sharing a parallel group run together;
        # non-grouped steps run individually.
        groups = _group_steps(plan.steps)

        for group_steps in groups:
            if len(group_steps) > 1 or group_steps[0].parallel:
                results = self._step_runner._run_parallel_group(
                    group_steps, plan.variables
                )
            else:
                result = self._run_step(group_steps[0])
                results = [result]

            all_results.extend(results)

            # Check for failures and apply on_failure strategy
            failures = [r for r in results if not r.success]
            if failures:
                self._apply_failure_strategy(
                    plan.on_failure, failures, all_results
                )

        return all_results

    def _run_step(self, step: StepConfig) -> FaultResult:
        """Execute a single step through the registry and executor.

        Resolves the fault type from the registry, builds a FaultConfig,
        applies any pre-step delay, and delegates to the executor.
        """
        if step.delay_before > 0:
            logger.debug(
                "Delaying %.1fs before step '%s'", step.delay_before, step.name
            )
            time.sleep(step.delay_before)

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
            params=dict(step.args),
            duration=step.duration,
            dry_run=self._context.dry_run,
            experiment_name=None,
        )

        logger.info(
            "Executing step '%s' (%s/%s)", step.name, step.domain, step.fault
        )
        return self._executor.execute(fault_type, config)

    def _resolve_variables(
        self, args: dict[str, Any], variables: dict[str, Any]
    ) -> dict[str, Any]:
        """Resolve ${variable} references in step arguments."""
        return _resolve_variables(args, variables)

    def _apply_failure_strategy(
        self,
        strategy: OnFailure,
        failures: list[FaultResult],
        all_results: list[FaultResult],
    ) -> None:
        """Apply the configured on-failure strategy."""
        if strategy == OnFailure.ABORT:
            logger.error(
                "Step failed and on_failure=abort — aborting experiment"
            )
            raise _AbortExperiment(all_results)

        if strategy == OnFailure.ROLLBACK_AND_CONTINUE:
            logger.warning(
                "Step failed — rolling back failed faults and continuing"
            )
            for fr in failures:
                if fr.fault_id:
                    success = self._executor.rollback_one(fr.fault_id)
                    fr.rollback_success = success

        # OnFailure.CONTINUE — just keep going
        if strategy == OnFailure.CONTINUE:
            logger.warning("Step failed — continuing per on_failure=continue")

    def _handle_rollback(self, strategy: RollbackStrategy) -> None:
        """Execute rollback according to the configured strategy."""
        if strategy == RollbackStrategy.IMMEDIATE:
            count = self._executor.rollback_all()
            logger.info("Immediate rollback: %d faults rolled back", count)
        elif strategy == RollbackStrategy.END:
            count = self._executor.rollback_all()
            logger.info("End-of-experiment rollback: %d faults rolled back", count)
        elif strategy == RollbackStrategy.MANUAL:
            logger.info(
                "Rollback strategy is MANUAL — skipping automatic rollback. "
                "Active faults: %d",
                self._executor.active_count(),
            )


class _AbortExperiment(Exception):
    """Internal signal to abort an experiment run."""

    def __init__(self, results: list[FaultResult]) -> None:
        self.results = results
        super().__init__("Experiment aborted due to step failure")


def _group_steps(steps: list[StepConfig]) -> list[list[StepConfig]]:
    """Partition steps into sequential singletons and parallel groups.

    Consecutive steps that share the same non-None ``group`` value are
    collected into a single list.  All other steps become single-element
    lists.
    """
    groups: list[list[StepConfig]] = []
    current_group: list[StepConfig] = []
    current_group_name: Optional[str] = None

    for step in steps:
        if step.group is not None and step.group == current_group_name:
            current_group.append(step)
        else:
            if current_group:
                groups.append(current_group)
            if step.group is not None:
                current_group = [step]
                current_group_name = step.group
            else:
                groups.append([step])
                current_group = []
                current_group_name = None

    if current_group:
        groups.append(current_group)

    return groups
