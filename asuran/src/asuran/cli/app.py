"""Main CLI entry point for Asuran."""
from __future__ import annotations

import argparse
import logging
import os
import socket
import sys

from asuran.version import __version__


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="asuran",
        description="Asuran — Linux Chaos Engineering Agent",
    )
    parser.add_argument("--version", action="version", version=f"asuran {__version__}")
    parser.add_argument("-v", "--verbose", action="count", default=0, help="Verbose output (-vv for trace)")
    parser.add_argument("--dry-run", action="store_true", help="Dry-run mode")
    parser.add_argument("--config", help="Config file path")
    parser.add_argument("--db", help="SQLite database path")
    parser.add_argument("command", nargs="*", help="Single command to execute (omit for REPL)")
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    # Setup logging
    level = logging.WARNING
    if args.verbose >= 2:
        level = logging.DEBUG
    elif args.verbose >= 1:
        level = logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    from asuran.core.context import ExecutionContext
    from asuran.core.executor import FaultExecutor
    from asuran.core.registry import PluginRegistry
    from asuran.safety.limits import BlastRadiusGuard
    from asuran.safety.rollback import RollbackManager
    from asuran.telemetry.audit import AuditTrail

    # Initialize components
    registry = PluginRegistry()
    registry.discover()

    rollback_mgr = RollbackManager()
    safety_guard = BlastRadiusGuard()
    audit = AuditTrail()

    context = ExecutionContext(
        rollback_manager=rollback_mgr,
        dry_run=args.dry_run,
        verbose=args.verbose,
    )

    executor = FaultExecutor(
        context=context,
        safety_guard=safety_guard,
    )

    # Check for crash recovery
    orphans = rollback_mgr.recover_from_crash()
    if orphans:
        print(f"\n  Found {len(orphans)} orphaned fault(s) from previous session:")
        for o in orphans:
            print(f"    - {o.get('domain', '?')}/{o.get('fault_type', '?')}: {o.get('description', '')}")
        rollback_mgr.clear_state()
        print()

    # Print banner
    domain_names = registry.domain_names()
    print(f"Asuran v{__version__} | Python {sys.version.split()[0]} | {socket.gethostname()}")
    print(f"Loaded {len(domain_names)} domains: {' '.join(domain_names)}")
    if args.dry_run:
        print("[DRY RUN MODE]")
    print()

    if args.command:
        # Single command mode
        from asuran.cli.repl import execute_single_command
        execute_single_command(args.command, registry, executor, context, audit)
    else:
        # Interactive REPL
        from asuran.cli.repl import run_repl
        run_repl(registry, executor, context, audit, rollback_mgr)


if __name__ == "__main__":
    main()
