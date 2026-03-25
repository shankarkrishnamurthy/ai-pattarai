"""System metrics collector — reads /proc and /sys for baseline/during/after metrics."""
from __future__ import annotations

import os
import time
from pathlib import Path
from typing import Any


class MetricsCollector:
    """Collects system metrics from /proc and /sys."""

    def collect_all(self) -> dict[str, float]:
        metrics: dict[str, float] = {}
        metrics.update(self.collect_cpu())
        metrics.update(self.collect_memory())
        metrics.update(self.collect_load())
        metrics.update(self.collect_disk_io())
        metrics.update(self.collect_net_io())
        return metrics

    def collect_cpu(self) -> dict[str, float]:
        try:
            with open("/proc/stat") as f:
                line = f.readline()
            parts = line.split()
            if parts[0] == "cpu":
                vals = [int(x) for x in parts[1:]]
                total = sum(vals)
                idle = vals[3] if len(vals) > 3 else 0
                usage = ((total - idle) / total * 100) if total else 0
                return {"sys.cpu.percent": round(usage, 2)}
        except Exception:
            pass
        return {}

    def collect_memory(self) -> dict[str, float]:
        try:
            info: dict[str, int] = {}
            with open("/proc/meminfo") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 2:
                        key = parts[0].rstrip(":")
                        info[key] = int(parts[1]) * 1024  # kB to bytes
            total = info.get("MemTotal", 1)
            available = info.get("MemAvailable", 0)
            used_pct = ((total - available) / total * 100) if total else 0
            swap_total = info.get("SwapTotal", 0)
            swap_free = info.get("SwapFree", 0)
            return {
                "sys.mem.total": total,
                "sys.mem.available": available,
                "sys.mem.percent": round(used_pct, 2),
                "sys.mem.swap_used": swap_total - swap_free,
            }
        except Exception:
            return {}

    def collect_load(self) -> dict[str, float]:
        try:
            with open("/proc/loadavg") as f:
                parts = f.read().split()
            return {
                "sys.cpu.load_1m": float(parts[0]),
                "sys.cpu.load_5m": float(parts[1]),
                "sys.cpu.load_15m": float(parts[2]),
            }
        except Exception:
            return {}

    def collect_disk_io(self) -> dict[str, float]:
        try:
            metrics: dict[str, float] = {}
            with open("/proc/diskstats") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 14:
                        dev = parts[2]
                        if dev.startswith("sd") or dev.startswith("nvme") or dev.startswith("vd"):
                            if not any(c.isdigit() for c in dev[2:]) or dev.startswith("nvme"):
                                metrics[f"sys.disk.{dev}.read_ios"] = float(parts[3])
                                metrics[f"sys.disk.{dev}.write_ios"] = float(parts[7])
            return metrics
        except Exception:
            return {}

    def collect_net_io(self) -> dict[str, float]:
        try:
            metrics: dict[str, float] = {}
            net_dir = Path("/sys/class/net")
            if net_dir.exists():
                for iface_dir in net_dir.iterdir():
                    iface = iface_dir.name
                    if iface == "lo":
                        continue
                    stats_dir = iface_dir / "statistics"
                    for stat in ("rx_bytes", "tx_bytes", "rx_packets", "tx_packets",
                                 "rx_errors", "rx_dropped"):
                        stat_file = stats_dir / stat
                        if stat_file.exists():
                            val = stat_file.read_text().strip()
                            metrics[f"sys.net.{iface}.{stat}"] = float(val)
            return metrics
        except Exception:
            return {}
