"""Output formatting for Asuran CLI."""
from __future__ import annotations

import time
from typing import Any

from rich.console import Console
from rich.panel import Panel
from rich.table import Table
from rich.text import Text

console = Console()


def print_fault_result(result: Any) -> None:
    """Print a fault injection result."""
    if result.success:
        status = Text("[OK]", style="bold green")
    else:
        status = Text("[FAIL]", style="bold red")

    console.print(
        status,
        f" {result.domain}/{result.fault_type}",
        f"(fault_id: {result.fault_id})",
    )
    if result.error:
        console.print(f"  Error: {result.error}", style="red")


def print_active_faults(faults: list[dict]) -> None:
    """Print table of active faults."""
    if not faults:
        console.print("No active faults.", style="dim")
        return

    table = Table(title="Active Faults", show_lines=True)
    table.add_column("ID", style="cyan", no_wrap=True)
    table.add_column("Domain", style="blue")
    table.add_column("Type", style="green")
    table.add_column("Description")
    table.add_column("Elapsed", style="yellow")

    for f in faults:
        elapsed = ""
        if "start_time" in f:
            elapsed = f"{time.time() - f['start_time']:.0f}s"
        table.add_row(
            f.get("fault_id", "?")[:8],
            f.get("domain", "?"),
            f.get("fault_type", "?"),
            f.get("description", ""),
            elapsed,
        )

    console.print(table)


def print_help(registry: Any) -> None:
    """Print help text."""
    console.print(Panel(
        "[bold]Asuran[/bold] — Linux Chaos Engineering Agent\n\n"
        "Commands:\n"
        "  [cyan]<domain> <fault> [args] [--flags][/cyan]  Inject a fault\n"
        "  [cyan]status[/cyan]                              Show active faults\n"
        "  [cyan]recover [fault-id][/cyan]                  Roll back faults\n"
        "  [cyan]history[/cyan]                             Show experiment history\n"
        "  [cyan]help [domain][/cyan]                       Show help\n"
        "  [cyan]exit / quit[/cyan]                         Exit (rolls back all)\n",
        title="Help",
    ))

    tree = registry.command_tree()
    if tree:
        table = Table(title="Available Domains")
        table.add_column("Domain", style="cyan")
        table.add_column("Fault Types", style="green")
        for domain, faults in sorted(tree.items()):
            table.add_row(domain, ", ".join(faults))
        console.print(table)


def print_domain_help(domain: Any) -> None:
    """Print help for a specific domain."""
    console.print(f"\n[bold cyan]{domain.name}[/bold cyan] — {domain.description}")
    tools = domain.required_tools()
    if tools:
        console.print(f"  Required tools: {', '.join(tools)}")
    caps = domain.required_capabilities()
    if caps:
        console.print(f"  Required capabilities: {', '.join(caps)}")
    console.print()
    for ft in domain.fault_types():
        console.print(f"  [green]{ft.name}[/green] — {ft.description} [dim](severity: {ft.severity.name})[/dim]")
        for p in ft.parameters():
            req = " [required]" if p.required else ""
            default = f" (default: {p.default})" if p.default is not None else ""
            unit = f" [{p.unit}]" if p.unit else ""
            console.print(f"    --{p.name}{unit}{req}{default}: {p.help}")
        console.print()
