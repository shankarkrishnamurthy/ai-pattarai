"""Plugin discovery and registration engine."""
from __future__ import annotations

import importlib
import logging
from pathlib import Path
from typing import Optional

from asuran.core.errors import PluginError
from asuran.core.plugin import ChaosDomain, FaultType

logger = logging.getLogger(__name__)

# Built-in domain package names
BUILTIN_DOMAINS = [
    "network", "cpu", "memory", "disk", "process", "kernel",
    "time", "filesystem", "services", "crypto", "hardware",
    "boot", "dns_domain", "software",
]


class PluginRegistry:
    """Discovers and registers all ChaosDomain plugins."""

    def __init__(self) -> None:
        self._domains: dict[str, ChaosDomain] = {}

    def discover(self, extra_dirs: Optional[list[str]] = None) -> None:
        """Run all discovery layers."""
        self._discover_builtins()
        self._discover_entry_points()
        if extra_dirs:
            for d in extra_dirs:
                self._discover_directory(Path(d))

    def _discover_builtins(self) -> None:
        for name in BUILTIN_DOMAINS:
            try:
                mod = importlib.import_module(f"asuran.domains.{name}.plugin")
                domain = mod.create_domain()
                self.register(domain)
            except Exception as e:
                logger.warning("Failed to load built-in domain %s: %s", name, e)

    def _discover_entry_points(self) -> None:
        try:
            from importlib.metadata import entry_points
            eps = entry_points()
            group = eps.get("asuran.domains", []) if isinstance(eps, dict) else eps.select(group="asuran.domains")
            for ep in group:
                try:
                    create_fn = ep.load()
                    domain = create_fn()
                    self.register(domain)
                except Exception as e:
                    logger.warning("Failed to load entry point %s: %s", ep.name, e)
        except Exception:
            pass

    def _discover_directory(self, path: Path) -> None:
        if not path.is_dir():
            return
        for f in path.glob("*.py"):
            try:
                import importlib.util
                spec = importlib.util.spec_from_file_location(f.stem, f)
                if spec and spec.loader:
                    mod = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(mod)
                    if hasattr(mod, "create_domain"):
                        domain = mod.create_domain()
                        self.register(domain)
            except Exception as e:
                logger.warning("Failed to load plugin %s: %s", f, e)

    def register(self, domain: ChaosDomain) -> None:
        if not isinstance(domain, ChaosDomain):
            raise PluginError(f"Expected ChaosDomain, got {type(domain)}")
        self._domains[domain.name] = domain
        logger.info("Registered domain: %s", domain.name)

    def get_domain(self, name: str) -> Optional[ChaosDomain]:
        return self._domains.get(name)

    def get_fault_type(self, domain_name: str, fault_name: str) -> Optional[FaultType]:
        domain = self.get_domain(domain_name)
        if not domain:
            return None
        for ft in domain.fault_types():
            if ft.name == fault_name:
                return ft
        return None

    def all_domains(self) -> list[ChaosDomain]:
        return list(self._domains.values())

    def domain_names(self) -> list[str]:
        return list(self._domains.keys())

    def command_tree(self) -> dict[str, list[str]]:
        """Return domain -> [fault_type_names] for tab completion."""
        tree: dict[str, list[str]] = {}
        for name, domain in self._domains.items():
            tree[name] = [ft.name for ft in domain.fault_types()]
        return tree
