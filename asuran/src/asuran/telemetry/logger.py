"""Structured JSON logger for Asuran."""
from __future__ import annotations

import json
import logging
import os
import socket
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Any, Optional


class JSONFormatter(logging.Formatter):
    """Outputs log records as JSON lines."""

    def format(self, record: logging.LogRecord) -> str:
        data = {
            "timestamp": self.formatTime(record),
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
            "hostname": socket.gethostname(),
            "pid": os.getpid(),
        }
        if hasattr(record, "event_type"):
            data["event_type"] = record.event_type
        if hasattr(record, "fault_id"):
            data["fault_id"] = record.fault_id
        if hasattr(record, "domain"):
            data["domain"] = record.domain
        if hasattr(record, "experiment_id"):
            data["experiment_id"] = record.experiment_id
        if record.exc_info and record.exc_info[1]:
            data["error"] = str(record.exc_info[1])
        return json.dumps(data)


class AsuranLogger:
    """Structured logging for Asuran events."""

    def __init__(
        self,
        log_file: Optional[str] = None,
        log_level: str = "INFO",
        log_format: str = "json",
        max_bytes: int = 50 * 1024 * 1024,
        backup_count: int = 10,
    ) -> None:
        self.logger = logging.getLogger("asuran")
        self.logger.setLevel(getattr(logging, log_level.upper(), logging.INFO))

        if log_file:
            log_path = Path(log_file).expanduser()
            log_path.parent.mkdir(parents=True, exist_ok=True)
            handler = RotatingFileHandler(
                log_path, maxBytes=max_bytes, backupCount=backup_count
            )
            if log_format == "json":
                handler.setFormatter(JSONFormatter())
            else:
                handler.setFormatter(logging.Formatter(
                    "%(asctime)s [%(levelname)s] %(name)s: %(message)s"
                ))
            self.logger.addHandler(handler)

    def event(
        self,
        event_type: str,
        message: str,
        level: str = "INFO",
        fault_id: Optional[str] = None,
        domain: Optional[str] = None,
        experiment_id: Optional[str] = None,
        **extra: Any,
    ) -> None:
        record_level = getattr(logging, level.upper(), logging.INFO)
        extra_attrs = {
            "event_type": event_type,
        }
        if fault_id:
            extra_attrs["fault_id"] = fault_id
        if domain:
            extra_attrs["domain"] = domain
        if experiment_id:
            extra_attrs["experiment_id"] = experiment_id

        self.logger.log(record_level, message, extra=extra_attrs)
