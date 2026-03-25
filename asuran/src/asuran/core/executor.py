"""Fault executor — manages the full lifecycle of fault injection."""
from __future__ import annotations

import logging
import threading
import time
import uuid
from typing import Any, Optional

from asuran.core.context import ExecutionContext
from asuran.core.errors import (
    BlastRadiusError,
    InjectionError,
    PreCheckError,
    ValidationError,
)
from asuran.core.plugin import FaultType
from asuran.core.types import FaultConfig, FaultResult, Phase

logger = logging.getLogger(__name__)


class FaultExecutor:
    """Executes faults through the full lifecycle:
    validate -> pre_check -> safety -> inject -> monitor -> rollback.
    """

    def __init__(
        self,
        context: ExecutionContext,
        safety_guard: Any = None,
        metrics_collector: Any = None,
    ) -> None:
        self.context = context
        self.safety_guard = safety_guard
        self.metrics_collector = metrics_collector
        self._active: dict[str, _ActiveFault] = {}
        self._timers: dict[str, threading.Timer] = {}

    def execute(self, fault_type: FaultType, config: FaultConfig) -> FaultResult:
        """Execute a fault through the full lifecycle."""
        start_time = time.time()
        fault_id = str(uuid.uuid4())[:8]
        result = FaultResult(
            fault_id=fault_id,
            success=False,
            phase=Phase.PENDING,
            start_time=start_time,
            domain=getattr(fault_type, '_domain_name', ''),
            fault_type=fault_type.name,
        )

        # 1. Validate
        result.phase = Phase.VALIDATING
        errors = fault_type.validate(config)
        if errors:
            result.phase = Phase.FAILED
            result.error = "; ".join(errors)
            result.end_time = time.time()
            return result

        # 2. Pre-check
        result.phase = Phase.PRE_CHECK
        warnings = fault_type.pre_check(config, self.context)
        if warnings:
            for w in warnings:
                logger.warning("Pre-check: %s", w)

        # 3. Safety guards
        if self.safety_guard:
            try:
                self.safety_guard.check(fault_type, config, len(self._active))
            except BlastRadiusError as e:
                result.phase = Phase.FAILED
                result.error = str(e)
                result.end_time = time.time()
                return result

        # 4. Inject
        result.phase = Phase.INJECTING
        try:
            actual_id = fault_type.inject(config, self.context)
            fault_id = actual_id or fault_id
            result.fault_id = fault_id
        except Exception as e:
            result.phase = Phase.FAILED
            result.error = str(e)
            result.end_time = time.time()
            logger.error("Injection failed: %s", e)
            return result

        # 5. Track active fault
        result.phase = Phase.ACTIVE
        result.success = True
        self._active[fault_id] = _ActiveFault(
            fault_id=fault_id,
            fault_type=fault_type,
            config=config,
            start_time=start_time,
        )

        # 6. Duration timer
        if config.duration and not config.dry_run:
            timer = threading.Timer(config.duration, self._auto_rollback, args=[fault_id])
            timer.daemon = True
            timer.start()
            self._timers[fault_id] = timer

        return result

    def rollback_one(self, fault_id: str) -> bool:
        """Roll back a specific fault."""
        active = self._active.get(fault_id)
        if not active:
            logger.warning("No active fault with id %s", fault_id)
            return False

        # Cancel timer
        timer = self._timers.pop(fault_id, None)
        if timer:
            timer.cancel()

        try:
            active.fault_type.rollback(fault_id, self.context)
            del self._active[fault_id]
            logger.info("Rolled back fault %s", fault_id)
            return True
        except Exception as e:
            logger.error("Rollback failed for %s: %s", fault_id, e)
            return False

    def rollback_all(self) -> int:
        """Roll back all active faults (LIFO order). Return count rolled back."""
        ids = list(reversed(list(self._active.keys())))
        count = 0
        for fid in ids:
            if self.rollback_one(fid):
                count += 1
        return count

    def active_faults(self) -> dict[str, _ActiveFault]:
        return dict(self._active)

    def active_count(self) -> int:
        return len(self._active)

    def _auto_rollback(self, fault_id: str) -> None:
        logger.info("Duration expired for fault %s, rolling back", fault_id)
        self.rollback_one(fault_id)


class _ActiveFault:
    __slots__ = ("fault_id", "fault_type", "config", "start_time")

    def __init__(self, fault_id: str, fault_type: FaultType, config: FaultConfig, start_time: float):
        self.fault_id = fault_id
        self.fault_type = fault_type
        self.config = config
        self.start_time = start_time
