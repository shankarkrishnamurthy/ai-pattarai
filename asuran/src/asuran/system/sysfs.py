"""/sys filesystem readers."""
from __future__ import annotations

from pathlib import Path


def get_network_interfaces() -> list[str]:
    net_dir = Path("/sys/class/net")
    if not net_dir.exists():
        return []
    return [d.name for d in net_dir.iterdir() if d.name != "lo"]


def get_interface_state(iface: str) -> str:
    try:
        return Path(f"/sys/class/net/{iface}/operstate").read_text().strip()
    except Exception:
        return "unknown"


def get_block_devices() -> list[str]:
    block_dir = Path("/sys/block")
    if not block_dir.exists():
        return []
    return [d.name for d in block_dir.iterdir()]
