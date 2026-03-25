"""cgroup v1/v2 manipulation."""
from __future__ import annotations

import os
from pathlib import Path


def detect_cgroup_version() -> int:
    if Path("/sys/fs/cgroup/cgroup.controllers").exists():
        return 2
    if Path("/sys/fs/cgroup/cpu").exists():
        return 1
    return 0


def create_cgroup(name: str, controller: str = "cpu") -> Path:
    version = detect_cgroup_version()
    if version == 2:
        path = Path(f"/sys/fs/cgroup/{name}")
    else:
        path = Path(f"/sys/fs/cgroup/{controller}/{name}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def set_cpu_quota(cgroup_path: Path, quota_us: int, period_us: int = 100000) -> None:
    version = detect_cgroup_version()
    if version == 2:
        (cgroup_path / "cpu.max").write_text(f"{quota_us} {period_us}")
    else:
        (cgroup_path / "cpu.cfs_quota_us").write_text(str(quota_us))
        (cgroup_path / "cpu.cfs_period_us").write_text(str(period_us))


def add_pid_to_cgroup(cgroup_path: Path, pid: int) -> None:
    procs_file = cgroup_path / "cgroup.procs"
    if not procs_file.exists():
        procs_file = cgroup_path / "tasks"
    procs_file.write_text(str(pid))


def remove_cgroup(cgroup_path: Path) -> None:
    try:
        cgroup_path.rmdir()
    except OSError:
        pass
