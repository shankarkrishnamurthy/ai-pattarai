"""Network chaos domain — latency, packet loss, corruption, bandwidth, blackhole, DNS failure.

Uses Linux traffic control (tc/netem) and iptables to inject network faults.
"""
from __future__ import annotations

import uuid
from typing import Any

from asuran.core.plugin import ChaosDomain, FaultType
from asuran.core.types import FaultConfig, ParameterSpec, Severity, Phase


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_TC_ROLLBACK_CMD = ["tc", "qdisc", "del", "dev", "{iface}", "root"]


def _tc_rollback_cmd(iface: str) -> list[str]:
    """Build the tc qdisc delete command for a given interface."""
    return ["tc", "qdisc", "del", "dev", iface, "root"]


# ---------------------------------------------------------------------------
# Fault types
# ---------------------------------------------------------------------------


class DelayFault(FaultType):
    """Inject network latency via tc/netem."""

    @property
    def name(self) -> str:
        return "delay"

    @property
    def description(self) -> str:
        return "Add network latency (and optional jitter) to an interface"

    @property
    def severity(self) -> Severity:
        return Severity.LOW

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="interface", type=str, required=True, positional=True,
                help="Network interface to target (e.g. eth0)",
            ),
            ParameterSpec(
                name="latency", type=int, required=True, unit="ms",
                help="Base latency to add in milliseconds",
            ),
            ParameterSpec(
                name="jitter", type=int, required=False, default=0, unit="ms",
                help="Jitter (+/-) in milliseconds",
            ),
            ParameterSpec(
                name="duration", type=float, required=False, default=None,
                help="Duration in seconds before automatic rollback",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        params = config.params
        if "interface" not in params or not params["interface"]:
            errors.append("'interface' is required")
        if "latency" not in params:
            errors.append("'latency' is required")
        elif not isinstance(params["latency"], (int, float)) or params["latency"] < 0:
            errors.append("'latency' must be a non-negative integer (ms)")
        jitter = params.get("jitter", 0)
        if not isinstance(jitter, (int, float)) or jitter < 0:
            errors.append("'jitter' must be a non-negative integer (ms)")
        return errors

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        warnings: list[str] = []
        if not context.check_tool("tc"):
            warnings.append("'tc' (iproute2) is not installed or not in PATH")
        return warnings

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        params = config.params
        iface = params["interface"]
        latency = params["latency"]
        jitter = params.get("jitter", 0)

        cmd = [
            "tc", "qdisc", "add", "dev", iface, "root", "netem",
            "delay", f"{latency}ms", f"{jitter}ms",
        ]

        if not config.dry_run:
            context.run_cmd(cmd)
        else:
            context.run_cmd(cmd)  # context.run_cmd handles dry_run internally

        rollback_cmd = _tc_rollback_cmd(iface)
        context.rollback_manager.push(
            fault_id=fault_id,
            rollback_fn=lambda: context.run_cmd(rollback_cmd),
            description=f"Remove netem delay on {iface}",
            domain="net",
            fault_type=self.name,
            rollback_cmd=rollback_cmd,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class LossFault(FaultType):
    """Inject packet loss via tc/netem."""

    @property
    def name(self) -> str:
        return "loss"

    @property
    def description(self) -> str:
        return "Drop a percentage of packets on an interface"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="interface", type=str, required=True, positional=True,
                help="Network interface to target (e.g. eth0)",
            ),
            ParameterSpec(
                name="percent", type=float, required=True, unit="%",
                help="Percentage of packets to drop (0-100)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        params = config.params
        if "interface" not in params or not params["interface"]:
            errors.append("'interface' is required")
        if "percent" not in params:
            errors.append("'percent' is required")
        elif not isinstance(params["percent"], (int, float)) or not (0 <= params["percent"] <= 100):
            errors.append("'percent' must be a number between 0 and 100")
        return errors

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        warnings: list[str] = []
        if not context.check_tool("tc"):
            warnings.append("'tc' (iproute2) is not installed or not in PATH")
        return warnings

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        params = config.params
        iface = params["interface"]
        percent = params["percent"]

        cmd = [
            "tc", "qdisc", "add", "dev", iface, "root", "netem",
            "loss", f"{percent}%",
        ]

        context.run_cmd(cmd)

        rollback_cmd = _tc_rollback_cmd(iface)
        context.rollback_manager.push(
            fault_id=fault_id,
            rollback_fn=lambda: context.run_cmd(rollback_cmd),
            description=f"Remove netem loss on {iface}",
            domain="net",
            fault_type=self.name,
            rollback_cmd=rollback_cmd,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class CorruptFault(FaultType):
    """Inject packet corruption via tc/netem."""

    @property
    def name(self) -> str:
        return "corrupt"

    @property
    def description(self) -> str:
        return "Corrupt a percentage of packets on an interface"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="interface", type=str, required=True, positional=True,
                help="Network interface to target (e.g. eth0)",
            ),
            ParameterSpec(
                name="percent", type=float, required=True, unit="%",
                help="Percentage of packets to corrupt (0-100)",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        params = config.params
        if "interface" not in params or not params["interface"]:
            errors.append("'interface' is required")
        if "percent" not in params:
            errors.append("'percent' is required")
        elif not isinstance(params["percent"], (int, float)) or not (0 <= params["percent"] <= 100):
            errors.append("'percent' must be a number between 0 and 100")
        return errors

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        warnings: list[str] = []
        if not context.check_tool("tc"):
            warnings.append("'tc' (iproute2) is not installed or not in PATH")
        return warnings

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        params = config.params
        iface = params["interface"]
        percent = params["percent"]

        cmd = [
            "tc", "qdisc", "add", "dev", iface, "root", "netem",
            "corrupt", f"{percent}%",
        ]

        context.run_cmd(cmd)

        rollback_cmd = _tc_rollback_cmd(iface)
        context.rollback_manager.push(
            fault_id=fault_id,
            rollback_fn=lambda: context.run_cmd(rollback_cmd),
            description=f"Remove netem corrupt on {iface}",
            domain="net",
            fault_type=self.name,
            rollback_cmd=rollback_cmd,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class BandwidthFault(FaultType):
    """Limit bandwidth via tc token bucket filter (tbf)."""

    @property
    def name(self) -> str:
        return "bandwidth"

    @property
    def description(self) -> str:
        return "Throttle bandwidth on an interface using token bucket filter"

    @property
    def severity(self) -> Severity:
        return Severity.MEDIUM

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="interface", type=str, required=True, positional=True,
                help="Network interface to target (e.g. eth0)",
            ),
            ParameterSpec(
                name="rate", type=str, required=True,
                help="Bandwidth rate limit (e.g. '1mbit', '500kbit')",
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        params = config.params
        if "interface" not in params or not params["interface"]:
            errors.append("'interface' is required")
        if "rate" not in params or not params["rate"]:
            errors.append("'rate' is required (e.g. '1mbit')")
        return errors

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        warnings: list[str] = []
        if not context.check_tool("tc"):
            warnings.append("'tc' (iproute2) is not installed or not in PATH")
        return warnings

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        params = config.params
        iface = params["interface"]
        rate = params["rate"]

        cmd = [
            "tc", "qdisc", "add", "dev", iface, "root", "tbf",
            "rate", rate, "burst", "32kbit", "latency", "400ms",
        ]

        context.run_cmd(cmd)

        rollback_cmd = _tc_rollback_cmd(iface)
        context.rollback_manager.push(
            fault_id=fault_id,
            rollback_fn=lambda: context.run_cmd(rollback_cmd),
            description=f"Remove tbf rate limit on {iface}",
            domain="net",
            fault_type=self.name,
            rollback_cmd=rollback_cmd,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class BlackholeFault(FaultType):
    """Drop all traffic to a destination via iptables."""

    @property
    def name(self) -> str:
        return "blackhole"

    @property
    def description(self) -> str:
        return "Drop all outbound traffic to a destination (blackhole)"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return [
            ParameterSpec(
                name="dst", type=str, required=True,
                help="Destination IP or CIDR to blackhole",
            ),
            ParameterSpec(
                name="port", type=int, required=False, default=None,
                help="Destination port (optional, limits to specific port)",
            ),
            ParameterSpec(
                name="protocol", type=str, required=False, default="tcp",
                help="Protocol (tcp/udp)", choices=["tcp", "udp"],
            ),
        ]

    def validate(self, config: FaultConfig) -> list[str]:
        errors: list[str] = []
        params = config.params
        if "dst" not in params or not params["dst"]:
            errors.append("'dst' (destination) is required")
        protocol = params.get("protocol", "tcp")
        if protocol not in ("tcp", "udp"):
            errors.append("'protocol' must be 'tcp' or 'udp'")
        port = params.get("port")
        if port is not None:
            if not isinstance(port, int) or not (1 <= port <= 65535):
                errors.append("'port' must be an integer between 1 and 65535")
        return errors

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        warnings: list[str] = []
        if not context.check_tool("iptables"):
            warnings.append("'iptables' is not installed or not in PATH")
        return warnings

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())
        params = config.params
        dst = params["dst"]
        port = params.get("port")
        protocol = params.get("protocol", "tcp")

        cmd = ["iptables", "-A", "OUTPUT", "-d", dst]
        if port is not None:
            cmd.extend(["-p", protocol, "--dport", str(port)])
        cmd.extend(["-j", "DROP"])

        context.run_cmd(cmd)

        # Build the matching rollback command (-D instead of -A)
        rollback_cmd = ["iptables", "-D", "OUTPUT", "-d", dst]
        if port is not None:
            rollback_cmd.extend(["-p", protocol, "--dport", str(port)])
        rollback_cmd.extend(["-j", "DROP"])

        context.rollback_manager.push(
            fault_id=fault_id,
            rollback_fn=lambda: context.run_cmd(rollback_cmd),
            description=f"Remove iptables blackhole for {dst}",
            domain="net",
            fault_type=self.name,
            rollback_cmd=rollback_cmd,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


class DnsFailFault(FaultType):
    """Block all DNS traffic via iptables (port 53 UDP)."""

    @property
    def name(self) -> str:
        return "dns-fail"

    @property
    def description(self) -> str:
        return "Block all outbound DNS queries (UDP port 53)"

    @property
    def severity(self) -> Severity:
        return Severity.HIGH

    def parameters(self) -> list[ParameterSpec]:
        return []

    def validate(self, config: FaultConfig) -> list[str]:
        return []

    def pre_check(self, config: FaultConfig, context: Any) -> list[str]:
        warnings: list[str] = []
        if not context.check_tool("iptables"):
            warnings.append("'iptables' is not installed or not in PATH")
        return warnings

    def inject(self, config: FaultConfig, context: Any) -> str:
        fault_id = str(uuid.uuid4())

        cmd = ["iptables", "-A", "OUTPUT", "-p", "udp", "--dport", "53", "-j", "DROP"]
        context.run_cmd(cmd)

        rollback_cmd = ["iptables", "-D", "OUTPUT", "-p", "udp", "--dport", "53", "-j", "DROP"]
        context.rollback_manager.push(
            fault_id=fault_id,
            rollback_fn=lambda: context.run_cmd(rollback_cmd),
            description="Remove iptables DNS block (udp/53)",
            domain="net",
            fault_type=self.name,
            rollback_cmd=rollback_cmd,
        )
        return fault_id

    def rollback(self, fault_id: str, context: Any) -> None:
        context.rollback_manager.rollback_one(fault_id)


# ---------------------------------------------------------------------------
# Domain
# ---------------------------------------------------------------------------


class NetworkDomain(ChaosDomain):
    """Network chaos domain — tc/netem and iptables based fault injection."""

    @property
    def name(self) -> str:
        return "net"

    @property
    def description(self) -> str:
        return "Network chaos: latency, loss, corruption, blackhole"

    def fault_types(self) -> list[FaultType]:
        return [
            DelayFault(),
            LossFault(),
            CorruptFault(),
            BandwidthFault(),
            BlackholeFault(),
            DnsFailFault(),
        ]

    def required_tools(self) -> list[str]:
        return ["tc", "iptables"]

    def required_capabilities(self) -> list[str]:
        return ["CAP_NET_ADMIN"]


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def create_domain() -> NetworkDomain:
    """Factory function called by the plugin registry."""
    return NetworkDomain()
