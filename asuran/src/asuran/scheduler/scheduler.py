"""Experiment scheduling with cron and interval triggers."""
from __future__ import annotations

import logging
import threading
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from asuran.engine.experiment import ExperimentEngine

logger = logging.getLogger(__name__)


@dataclass
class ScheduledJob:
    """A scheduled experiment job."""

    id: str
    plan_path: str
    cron_expr: Optional[str] = None
    interval_seconds: Optional[float] = None
    enabled: bool = True
    last_run: Optional[float] = None
    next_run: Optional[float] = None
    run_count: int = 0


def _cron_matches(expr: str, dt: datetime) -> bool:
    """Check whether a 5-field cron expression matches a given datetime.

    Supports basic cron syntax: five space-separated fields for
    minute, hour, day-of-month, month, and day-of-week.
    Each field may be ``*`` (any) or an integer value.

    Day-of-week uses 0=Monday .. 6=Sunday (ISO convention) for integer
    values, matching Python's ``datetime.weekday()``.
    """
    fields = expr.strip().split()
    if len(fields) != 5:
        raise ValueError(f"Cron expression must have 5 fields, got {len(fields)}: {expr!r}")

    actual_values = [
        dt.minute,
        dt.hour,
        dt.day,
        dt.month,
        dt.weekday(),  # 0=Monday .. 6=Sunday
    ]

    for field_str, actual in zip(fields, actual_values):
        if field_str == "*":
            continue
        try:
            if int(field_str) != actual:
                return False
        except ValueError:
            raise ValueError(
                f"Unsupported cron field value {field_str!r} in expression {expr!r}. "
                "Only '*' and integer literals are supported."
            )

    return True


class Scheduler:
    """Schedule experiment plans to run on cron expressions or fixed intervals.

    Parameters
    ----------
    engine:
        An ``ExperimentEngine`` instance used to execute plans.  The engine
        is expected to expose ``engine.run(plan) -> ExperimentResult`` and
        plans are loaded via ``asuran.engine.experiment.load_plan``.
    """

    _CHECK_INTERVAL: float = 30.0  # seconds between background-loop iterations

    def __init__(self, engine: ExperimentEngine) -> None:
        self._engine = engine
        self._jobs: dict[str, ScheduledJob] = {}
        self._lock = threading.Lock()
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def add(
        self,
        plan_path: str,
        cron_expr: Optional[str] = None,
        interval_seconds: Optional[float] = None,
    ) -> str:
        """Register a new scheduled job.  Returns the generated job ID.

        Exactly one of *cron_expr* or *interval_seconds* must be provided.
        """
        if not cron_expr and not interval_seconds:
            raise ValueError("Either cron_expr or interval_seconds must be provided")
        if cron_expr and interval_seconds:
            raise ValueError("Provide only one of cron_expr or interval_seconds, not both")

        job_id = str(uuid.uuid4())[:8]
        now = time.time()

        next_run: Optional[float] = None
        if interval_seconds is not None:
            next_run = now + interval_seconds

        job = ScheduledJob(
            id=job_id,
            plan_path=plan_path,
            cron_expr=cron_expr,
            interval_seconds=interval_seconds,
            next_run=next_run,
        )

        with self._lock:
            self._jobs[job_id] = job

        logger.info(
            "Added scheduled job %s for plan %s (cron=%s, interval=%s)",
            job_id, plan_path, cron_expr, interval_seconds,
        )
        return job_id

    def remove(self, job_id: str) -> bool:
        """Remove a job by ID.  Returns ``True`` if it existed."""
        with self._lock:
            return self._jobs.pop(job_id, None) is not None

    def list_jobs(self) -> list[ScheduledJob]:
        """Return a snapshot of all registered jobs."""
        with self._lock:
            return list(self._jobs.values())

    def pause(self, job_id: str) -> bool:
        """Disable a job so it is skipped during checks.  Returns ``True`` if found."""
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                return False
            job.enabled = False
            return True

    def resume(self, job_id: str) -> bool:
        """Re-enable a paused job.  Returns ``True`` if found."""
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                return False
            job.enabled = True
            return True

    # ------------------------------------------------------------------
    # Background scheduling loop
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start the background scheduler thread."""
        if self._running:
            logger.warning("Scheduler is already running")
            return

        self._stop_event.clear()
        self._running = True
        self._thread = threading.Thread(
            target=self._run_loop,
            name="asuran-scheduler",
            daemon=True,
        )
        self._thread.start()
        logger.info("Scheduler started")

    def stop(self) -> None:
        """Signal the background thread to stop and wait for it to finish."""
        if not self._running:
            return

        self._stop_event.set()
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=self._CHECK_INTERVAL + 5)
            self._thread = None
        logger.info("Scheduler stopped")

    def _run_loop(self) -> None:
        """Main loop executed in the background thread."""
        while not self._stop_event.is_set():
            try:
                self._check_jobs()
            except Exception:
                logger.exception("Error during scheduler job check")
            self._stop_event.wait(timeout=self._CHECK_INTERVAL)

    # ------------------------------------------------------------------
    # Job evaluation
    # ------------------------------------------------------------------

    def _check_jobs(self) -> None:
        """Iterate over all jobs and run any that are due."""
        with self._lock:
            jobs_snapshot = list(self._jobs.values())

        for job in jobs_snapshot:
            if not job.enabled:
                continue
            if self._should_run(job):
                self._execute_job(job)

    def _should_run(self, job: ScheduledJob) -> bool:
        """Determine whether *job* should be executed right now."""
        now = time.time()

        if job.interval_seconds is not None:
            if job.next_run is not None and now >= job.next_run:
                return True
            return False

        if job.cron_expr is not None:
            dt = datetime.now()
            try:
                return _cron_matches(job.cron_expr, dt)
            except ValueError as exc:
                logger.error("Invalid cron expression for job %s: %s", job.id, exc)
                return False

        return False

    def _execute_job(self, job: ScheduledJob) -> None:
        """Load the plan from *job.plan_path*, run it through the engine, and
        update book-keeping fields on the job."""
        try:
            from asuran.engine.experiment import load_plan

            plan = load_plan(job.plan_path)
            logger.info("Running scheduled job %s (plan: %s)", job.id, job.plan_path)
            result = self._engine.run(plan)
            logger.info(
                "Job %s completed (status=%s)",
                job.id,
                getattr(result, "status", "unknown"),
            )
        except Exception:
            logger.exception("Scheduled job %s failed", job.id)

        now = time.time()
        with self._lock:
            job.last_run = now
            job.run_count += 1
            if job.interval_seconds is not None:
                job.next_run = now + job.interval_seconds
