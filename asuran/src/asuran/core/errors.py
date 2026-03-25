"""Exception hierarchy for Asuran."""


class AsuranError(Exception):
    """Base exception for all Asuran errors."""


class PluginError(AsuranError):
    """Error in plugin discovery or loading."""


class ValidationError(AsuranError):
    """Fault configuration validation failed."""

    def __init__(self, errors: list[str]):
        self.errors = errors
        super().__init__(f"Validation failed: {'; '.join(errors)}")


class PreCheckError(AsuranError):
    """Pre-check requirements not met."""

    def __init__(self, warnings: list[str]):
        self.warnings = warnings
        super().__init__(f"Pre-check failed: {'; '.join(warnings)}")


class InjectionError(AsuranError):
    """Fault injection failed."""


class RollbackError(AsuranError):
    """Rollback operation failed."""


class SafetyError(AsuranError):
    """Safety guard blocked the operation."""


class BlastRadiusError(SafetyError):
    """Blast radius limits exceeded."""


class DurationError(SafetyError):
    """Duration limits exceeded."""


class SeverityError(SafetyError):
    """Severity too high for current configuration."""


class ExperimentError(AsuranError):
    """Experiment plan execution error."""


class ToolNotFoundError(AsuranError):
    """Required system tool not found."""

    def __init__(self, tool: str):
        self.tool = tool
        super().__init__(f"Required tool not found: {tool}")
