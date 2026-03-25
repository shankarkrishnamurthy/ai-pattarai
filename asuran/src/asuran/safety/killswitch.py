"""Kill switch — global abort mechanism."""
from __future__ import annotations

import logging
import os
import signal
import threading
from pathlib import Path
from typing import Callable, Optional

logger = logging.getLogger(__name__)


class KillSwitch:
    """Global abort mechanism with multiple triggers:
    1. SIGUSR1 to main process
    2. Touch file: /tmp/asuran_killswitch
    3. CLI command: recover
    4. Programmatic: activate()
    """

    def __init__(
        self,
        on_activate: Optional[Callable[[], None]] = None,
        killswitch_file: str = "/tmp/asuran_killswitch",
        poll_interval: float = 1.0,
    ) -> None:
        self._on_activate = on_activate
        self._killswitch_file = Path(killswitch_file)
        self._poll_interval = poll_interval
        self._activated = False
        self._poller: Optional[threading.Thread] = None
        self._stop_polling = threading.Event()
        self._setup_signal()

    def _setup_signal(self) -> None:
        try:
            signal.signal(signal.SIGUSR1, self._signal_handler)
        except (OSError, ValueError):
            pass

    def _signal_handler(self, signum: int, frame: object) -> None:
        logger.warning("SIGUSR1 received — kill switch activated")
        self.activate()

    def activate(self) -> None:
        if self._activated:
            return
        self._activated = True
        logger.warning("Kill switch ACTIVATED — rolling back all faults")
        if self._on_activate:
            self._on_activate()

    @property
    def is_activated(self) -> bool:
        return self._activated

    def start_polling(self) -> None:
        self._stop_polling.clear()
        self._poller = threading.Thread(target=self._poll_file, daemon=True)
        self._poller.start()

    def stop_polling(self) -> None:
        self._stop_polling.set()

    def _poll_file(self) -> None:
        while not self._stop_polling.is_set():
            if self._killswitch_file.exists():
                logger.warning("Kill switch file detected: %s", self._killswitch_file)
                try:
                    self._killswitch_file.unlink()
                except OSError:
                    pass
                self.activate()
                return
            self._stop_polling.wait(self._poll_interval)

    def reset(self) -> None:
        self._activated = False
