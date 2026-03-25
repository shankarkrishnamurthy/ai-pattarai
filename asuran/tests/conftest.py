"""Shared fixtures for Asuran tests."""
from __future__ import annotations

import pytest
from pathlib import Path

from asuran.core.context import ExecutionContext
from asuran.core.types import FaultConfig
from asuran.safety.rollback import RollbackManager


@pytest.fixture
def dry_context(tmp_path: Path) -> ExecutionContext:
    """An ExecutionContext with dry_run=True and a temporary rollback manager."""
    rm = RollbackManager(state_dir=tmp_path / ".asuran")
    return ExecutionContext(
        rollback_manager=rm,
        dry_run=True,
    )


@pytest.fixture
def rollback_mgr(tmp_path: Path) -> RollbackManager:
    """A RollbackManager using a temporary state directory."""
    return RollbackManager(state_dir=tmp_path / ".asuran")


@pytest.fixture
def dry_config() -> FaultConfig:
    """A minimal dry-run FaultConfig."""
    return FaultConfig(params={}, dry_run=True)
