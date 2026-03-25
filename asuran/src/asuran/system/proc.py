"""/proc filesystem readers."""
from __future__ import annotations

from pathlib import Path
from typing import Optional


def read_proc_file(path: str) -> str:
    try:
        return Path(path).read_text()
    except Exception:
        return ""


def get_pid_status(pid: int) -> Optional[dict]:
    content = read_proc_file(f"/proc/{pid}/status")
    if not content:
        return None
    info: dict[str, str] = {}
    for line in content.splitlines():
        parts = line.split(":", 1)
        if len(parts) == 2:
            info[parts[0].strip()] = parts[1].strip()
    return info


def pid_exists(pid: int) -> bool:
    return Path(f"/proc/{pid}").exists()


def get_all_pids() -> list[int]:
    pids = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            pids.append(int(entry.name))
    return pids


def find_pids_by_name(name: str) -> list[int]:
    pids = []
    for pid in get_all_pids():
        try:
            comm = Path(f"/proc/{pid}/comm").read_text().strip()
            if comm == name:
                pids.append(pid)
        except Exception:
            continue
    return pids
