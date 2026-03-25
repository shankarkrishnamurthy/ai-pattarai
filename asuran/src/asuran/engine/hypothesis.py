"""Steady-state hypothesis checking."""
from __future__ import annotations

import logging
import subprocess
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


@dataclass
class SteadyStateCheck:
    """A single steady-state assertion.

    Attributes:
        name: Human-readable label for the check.
        type: One of ``"http"``, ``"command"``, or ``"file"``.
        target: The URL, shell command, or file path to probe.
        expect: Expected outcome — keys depend on ``type``:
            - http:    ``{"status": 200}``
            - command: ``{"exit_code": 0}``  (default is 0 when omitted)
            - file:    ``{}``  (existence is the only assertion)
    """

    name: str
    type: str  # "http", "command", "file"
    target: str
    expect: dict[str, Any] = field(default_factory=dict)


def check_steady_state(
    checks: list[SteadyStateCheck],
    context: Any = None,
) -> tuple[bool, list[dict[str, Any]]]:
    """Run a list of steady-state checks and report results.

    Args:
        checks: Ordered list of checks to execute.
        context: Optional ``ExecutionContext`` (currently unused but
            available for future extension, e.g. dry-run awareness).

    Returns:
        A 2-tuple of ``(all_passed, details)`` where *details* is a list
        of per-check result dicts containing ``name``, ``passed``,
        ``type``, and ``message`` keys.
    """
    results: list[dict[str, Any]] = []
    all_passed = True

    for check in checks:
        logger.debug("Running steady-state check: %s (%s)", check.name, check.type)

        if check.type == "http":
            passed, message = _check_http(check)
        elif check.type == "command":
            passed, message = _check_command(check)
        elif check.type == "file":
            passed, message = _check_file(check)
        else:
            passed = False
            message = f"Unknown check type: {check.type}"

        if not passed:
            all_passed = False
            logger.warning(
                "Steady-state check FAILED: %s — %s", check.name, message
            )
        else:
            logger.debug("Steady-state check passed: %s", check.name)

        results.append(
            {
                "name": check.name,
                "passed": passed,
                "type": check.type,
                "message": message,
            }
        )

    return all_passed, results


def _check_http(check: SteadyStateCheck) -> tuple[bool, str]:
    """Perform an HTTP GET and validate the response status code.

    Expected keys in ``check.expect``:
        status (int): Required HTTP status code (default 200).
    """
    expected_status = check.expect.get("status", 200)
    url = check.target

    try:
        request = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(request, timeout=10) as response:
            actual_status = response.status
            if actual_status == expected_status:
                return True, f"HTTP {actual_status} (expected {expected_status})"
            return False, (
                f"HTTP {actual_status} (expected {expected_status})"
            )
    except urllib.error.HTTPError as exc:
        actual_status = exc.code
        if actual_status == expected_status:
            return True, f"HTTP {actual_status} (expected {expected_status})"
        return False, f"HTTP {actual_status} (expected {expected_status})"
    except urllib.error.URLError as exc:
        return False, f"Connection failed: {exc.reason}"
    except Exception as exc:
        return False, f"HTTP check error: {exc}"


def _check_command(check: SteadyStateCheck) -> tuple[bool, str]:
    """Run a shell command and validate the exit code.

    Expected keys in ``check.expect``:
        exit_code (int): Expected exit code (default 0).
    """
    expected_exit_code = check.expect.get("exit_code", 0)
    command = check.target

    try:
        completed = subprocess.run(
            command,
            shell=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        actual_code = completed.returncode
        if actual_code == expected_exit_code:
            return True, (
                f"Exit code {actual_code} (expected {expected_exit_code})"
            )
        stderr_snippet = (completed.stderr or "").strip()[:200]
        return False, (
            f"Exit code {actual_code} (expected {expected_exit_code})"
            + (f": {stderr_snippet}" if stderr_snippet else "")
        )
    except subprocess.TimeoutExpired:
        return False, "Command timed out after 30s"
    except Exception as exc:
        return False, f"Command check error: {exc}"


def _check_file(check: SteadyStateCheck) -> tuple[bool, str]:
    """Verify that a file exists at the given path."""
    target_path = Path(check.target)
    if target_path.exists():
        return True, f"File exists: {check.target}"
    return False, f"File not found: {check.target}"
