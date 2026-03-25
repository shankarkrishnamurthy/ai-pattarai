"""Interactive REPL engine using prompt_toolkit."""
from __future__ import annotations

import shlex
import time
from typing import Any

from prompt_toolkit import PromptSession
from prompt_toolkit.completion import WordCompleter
from prompt_toolkit.history import InMemoryHistory

from asuran.cli.formatter import (
    console,
    print_active_faults,
    print_domain_help,
    print_fault_result,
    print_help,
)
from asuran.core.types import FaultConfig


def _parse_value(raw: str, param_type: type) -> Any:
    """Parse a CLI value, handling units like 100ms, 10G."""
    if param_type in (int, float):
        # Strip unit suffixes
        units = {"ms": 1, "s": 1, "m": 60, "h": 3600,
                 "K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4,
                 "%": 1}
        for suffix, multiplier in units.items():
            if raw.endswith(suffix) and raw[:-len(suffix)].replace(".", "").isdigit():
                return param_type(float(raw[:-len(suffix)]))
        return param_type(raw)
    return raw


def _parse_command(tokens: list[str], registry: Any) -> tuple[Any, FaultConfig] | tuple[None, None]:
    """Parse tokens into (FaultType, FaultConfig)."""
    if len(tokens) < 2:
        return None, None

    domain_name = tokens[0]
    fault_name = tokens[1]

    fault_type = registry.get_fault_type(domain_name, fault_name)
    if not fault_type:
        return None, None

    params: dict[str, Any] = {}
    positional_idx = 0
    param_specs = fault_type.parameters()
    positional_specs = [p for p in param_specs if p.positional]
    named_specs = {f"--{p.name}": p for p in param_specs}

    i = 2
    duration = None
    dry_run = False
    tags: list[str] = []

    while i < len(tokens):
        token = tokens[i]

        if token == "--duration" and i + 1 < len(tokens):
            duration = _parse_value(tokens[i + 1], float)
            i += 2
        elif token == "--dry-run":
            dry_run = True
            i += 1
        elif token == "--tag" and i + 1 < len(tokens):
            tags.append(tokens[i + 1])
            i += 2
        elif token.startswith("--") and token in named_specs:
            spec = named_specs[token]
            if i + 1 < len(tokens):
                params[spec.name] = _parse_value(tokens[i + 1], spec.type)
                i += 2
            else:
                i += 1
        elif not token.startswith("--") and positional_idx < len(positional_specs):
            spec = positional_specs[positional_idx]
            params[spec.name] = _parse_value(token, spec.type)
            positional_idx += 1
            i += 1
        else:
            i += 1

    # Apply defaults
    for spec in param_specs:
        if spec.name not in params and spec.default is not None:
            params[spec.name] = spec.default

    config = FaultConfig(
        params=params,
        duration=duration,
        dry_run=dry_run,
        tags=tags,
    )
    return fault_type, config


def _build_completer(registry: Any) -> WordCompleter:
    """Build tab completer from registry."""
    words = []
    tree = registry.command_tree()
    words.extend(tree.keys())
    for faults in tree.values():
        words.extend(faults)
    words.extend([
        "status", "recover", "exit", "quit", "help", "history",
        "version", "clear", "env", "set", "unset",
        "--duration", "--dry-run", "--tag", "--verbose", "--json",
    ])
    return WordCompleter(words, ignore_case=True)


def execute_single_command(
    tokens: list[str],
    registry: Any,
    executor: Any,
    context: Any,
    audit: Any,
) -> None:
    """Execute a single command and exit."""
    cmd_str = " ".join(tokens)
    fault_type, config = _parse_command(tokens, registry)
    if fault_type and config:
        result = executor.execute(fault_type, config)
        print_fault_result(result)
        audit.record(cmd_str, "ok" if result.success else "fail",
                     fault_id=result.fault_id, domain=result.domain)
    else:
        console.print(f"Unknown command: {cmd_str}", style="red")


def run_repl(
    registry: Any,
    executor: Any,
    context: Any,
    audit: Any,
    rollback_mgr: Any,
) -> None:
    """Run the interactive REPL."""
    completer = _build_completer(registry)
    session: PromptSession = PromptSession(
        history=InMemoryHistory(),
        completer=completer,
    )

    while True:
        try:
            active = executor.active_count()
            prompt_str = f"asuran [{active} active] > "
            text = session.prompt(prompt_str).strip()
            if not text:
                continue

            audit.record(text)

            try:
                tokens = shlex.split(text)
            except ValueError as e:
                console.print(f"Parse error: {e}", style="red")
                continue

            cmd = tokens[0].lower()

            # Built-in commands
            if cmd in ("exit", "quit"):
                count = executor.rollback_all()
                if count:
                    console.print(f"Rolled back {count} fault(s).", style="yellow")
                console.print("Goodbye.", style="dim")
                break

            elif cmd == "status":
                faults = rollback_mgr.active_faults()
                print_active_faults(faults)

            elif cmd == "recover":
                if len(tokens) > 1:
                    success = executor.rollback_one(tokens[1])
                    if success:
                        console.print(f"Rolled back {tokens[1]}", style="green")
                    else:
                        console.print(f"Failed to roll back {tokens[1]}", style="red")
                else:
                    count = executor.rollback_all()
                    console.print(f"Rolled back {count} fault(s).", style="green")

            elif cmd == "help":
                if len(tokens) > 1:
                    domain = registry.get_domain(tokens[1])
                    if domain:
                        print_domain_help(domain)
                    else:
                        console.print(f"Unknown domain: {tokens[1]}", style="red")
                else:
                    print_help(registry)

            elif cmd == "history":
                try:
                    from asuran.telemetry.database import AsuranDB
                    db = AsuranDB()
                    exps = db.get_experiments()
                    if exps:
                        from rich.table import Table
                        table = Table(title="Experiment History")
                        table.add_column("ID", style="cyan")
                        table.add_column("Name")
                        table.add_column("Status")
                        table.add_column("Source")
                        for e in exps:
                            table.add_row(e["id"][:8], e["name"], e["status"], e["source"])
                        console.print(table)
                    else:
                        console.print("No experiments recorded.", style="dim")
                    db.close()
                except Exception:
                    console.print("No history available.", style="dim")

            elif cmd == "version":
                from asuran.version import __version__
                console.print(f"Asuran v{__version__}")

            elif cmd == "clear":
                console.clear()

            else:
                # Domain command
                fault_type, config = _parse_command(tokens, registry)
                if fault_type and config:
                    if context.dry_run:
                        config.dry_run = True
                    result = executor.execute(fault_type, config)
                    print_fault_result(result)
                    audit.record(text, "ok" if result.success else "fail",
                                 fault_id=result.fault_id, domain=result.domain)
                else:
                    console.print(f"Unknown command: {text}", style="red")
                    console.print("Type 'help' for available commands.", style="dim")

        except KeyboardInterrupt:
            console.print("\nUse 'exit' to quit (rolls back all faults).", style="yellow")
        except EOFError:
            count = executor.rollback_all()
            if count:
                console.print(f"\nRolled back {count} fault(s).", style="yellow")
            break
