"""
Error intelligence.

Nothing in this program raises a bare string. Every problem found during
analysis, conversion, validation or building becomes a `Finding`, which carries
enough context for a human (or the repair engine) to act on it:

    what asset it came from, what asset it was going to, how bad it is,
    why it probably happened, what to do about it, whether we can do that
    automatically, and everything we already tried.

The repair engine consumes findings and appends to `repair_history`, so a
finding is also the audit trail for its own fix.
"""

from __future__ import annotations

import enum
import itertools
import json
import time
from dataclasses import dataclass, field, asdict
from typing import Any, Iterable, Iterator


class CodportError(Exception):
    """Base class for errors that abort an operation."""


class ToolchainError(CodportError):
    """A required external tool is missing, or refused to run."""


class DetectionError(CodportError):
    """The source package could not be identified."""


class ParseError(CodportError):
    """A source file could not be parsed."""


class ConversionError(CodportError):
    """An asset could not be converted and has no fallback."""


class BuildError(CodportError):
    """The BO2 zone build failed in a way that is not recoverable."""


class Severity(enum.IntEnum):
    """Ordered so that `max()` over a set of findings gives the worst one."""

    INFO = 0
    NOTE = 10
    WARNING = 20
    ERROR = 30
    FATAL = 40

    @property
    def label(self) -> str:
        return self.name.title()

    @classmethod
    def parse(cls, value: str | int | Severity) -> Severity:
        if isinstance(value, Severity):
            return value
        if isinstance(value, int):
            return cls(value)
        return cls[str(value).strip().upper()]


class Category(enum.StrEnum):
    """
    Coarse bucket used for grouping in the UI and for routing to repair rules.

    Keep this list short. Specificity belongs in `Finding.code`, not here.
    """

    DETECTION = "detection"
    EXTRACTION = "extraction"
    DEPENDENCY = "dependency"
    MODEL = "model"
    MATERIAL = "material"
    IMAGE = "image"
    COLLISION = "collision"
    WORLD = "world"
    LIGHTING = "lighting"
    FX = "fx"
    SOUND = "sound"
    ENTITY = "entity"
    SCRIPT = "script"
    ZONE = "zone"
    BUILD = "build"
    VALIDATION = "validation"
    TOOLCHAIN = "toolchain"
    LEGAL = "legal"


class RepairOutcome(enum.StrEnum):
    ATTEMPTED = "attempted"
    REPAIRED = "repaired"
    FAILED = "failed"
    SKIPPED = "skipped"
    NOT_POSSIBLE = "not_possible"


@dataclass(slots=True)
class RepairAttempt:
    """One pass of the repair engine over one finding."""

    rule: str
    outcome: RepairOutcome
    detail: str = ""
    iteration: int = 0
    timestamp: float = field(default_factory=time.time)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["outcome"] = str(self.outcome)
        return d


_counter = itertools.count(1)


@dataclass(slots=True)
class Finding:
    """
    One actionable problem.

    `code` is the stable machine identifier (e.g. "MAT_NO_TECHSET_EQUIVALENT").
    Repair rules subscribe to codes, so renaming one is a breaking change.
    """

    code: str
    category: Category
    severity: Severity
    summary: str

    source_asset: str | None = None
    dest_asset: str | None = None
    likely_cause: str = ""
    suggested_fix: str = ""
    auto_repairable: bool = False
    confidence: float | None = None
    detail: dict[str, Any] = field(default_factory=dict)
    repair_history: list[RepairAttempt] = field(default_factory=list)
    number: int = field(default_factory=lambda: next(_counter))

    # ---- state -----------------------------------------------------------

    @property
    def repaired(self) -> bool:
        return any(a.outcome is RepairOutcome.REPAIRED for a in self.repair_history)

    @property
    def attempts(self) -> int:
        return len(self.repair_history)

    def record(
        self,
        rule: str,
        outcome: RepairOutcome,
        detail: str = "",
        iteration: int = 0,
    ) -> RepairAttempt:
        attempt = RepairAttempt(rule=rule, outcome=outcome, detail=detail, iteration=iteration)
        self.repair_history.append(attempt)
        return attempt

    # ---- rendering -------------------------------------------------------

    def to_dict(self) -> dict[str, Any]:
        return {
            "number": self.number,
            "code": self.code,
            "category": str(self.category),
            "severity": self.severity.label,
            "summary": self.summary,
            "source_asset": self.source_asset,
            "dest_asset": self.dest_asset,
            "likely_cause": self.likely_cause,
            "suggested_fix": self.suggested_fix,
            "auto_repairable": self.auto_repairable,
            "repaired": self.repaired,
            "confidence": self.confidence,
            "detail": self.detail,
            "repair_history": [a.to_dict() for a in self.repair_history],
        }

    def render(self) -> str:
        """Human-readable block, in the shape the spec asks for."""
        lines = [f"FINDING #{self.number}  [{self.severity.label}]  {self.code}"]
        if self.source_asset:
            lines.append(f"  Source asset : {self.source_asset}")
        if self.dest_asset:
            lines.append(f"  Dest asset   : {self.dest_asset}")
        lines.append(f"  Category     : {self.category}")
        lines.append(f"  Problem      : {self.summary}")
        if self.likely_cause:
            lines.append(f"  Likely cause : {self.likely_cause}")
        if self.suggested_fix:
            lines.append(f"  Fix          : {self.suggested_fix}")
        lines.append(f"  Auto repair  : {'YES' if self.auto_repairable else 'NO'}")
        if self.confidence is not None:
            lines.append(f"  Confidence   : {self.confidence * 100:.0f}%")
        for a in self.repair_history:
            lines.append(f"  - pass {a.iteration}: {a.rule} -> {a.outcome} {a.detail}".rstrip())
        return "\n".join(lines)


class FindingLog:
    """
    Ordered collection of findings with the query helpers the UI and the repair
    loop need. Deliberately not a plain list so that filtering stays readable.
    """

    def __init__(self, findings: Iterable[Finding] | None = None) -> None:
        self._items: list[Finding] = list(findings or ())

    # ---- collection protocol --------------------------------------------

    def __iter__(self) -> Iterator[Finding]:
        return iter(self._items)

    def __len__(self) -> int:
        return len(self._items)

    def __bool__(self) -> bool:
        return bool(self._items)

    def __getitem__(self, index: int) -> Finding:
        return self._items[index]

    # ---- building --------------------------------------------------------

    def add(self, finding: Finding) -> Finding:
        self._items.append(finding)
        return finding

    def emit(
        self,
        code: str,
        category: Category,
        severity: Severity,
        summary: str,
        **kwargs: Any,
    ) -> Finding:
        return self.add(
            Finding(
                code=code,
                category=category,
                severity=severity,
                summary=summary,
                **kwargs,
            )
        )

    def extend(self, other: Iterable[Finding]) -> None:
        self._items.extend(other)

    # ---- querying --------------------------------------------------------

    def by_severity(self, minimum: Severity) -> list[Finding]:
        return [f for f in self._items if f.severity >= minimum]

    def by_category(self, category: Category) -> list[Finding]:
        return [f for f in self._items if f.category is category]

    def by_code(self, code: str) -> list[Finding]:
        return [f for f in self._items if f.code == code]

    def unrepaired(self) -> list[Finding]:
        return [f for f in self._items if not f.repaired]

    def blocking(self) -> list[Finding]:
        """Findings that must be cleared before a build can be called good."""
        return [f for f in self._items if f.severity >= Severity.ERROR and not f.repaired]

    @property
    def worst(self) -> Severity:
        return max((f.severity for f in self._items), default=Severity.INFO)

    def counts(self) -> dict[str, int]:
        out: dict[str, int] = {s.label: 0 for s in Severity}
        for f in self._items:
            out[f.severity.label] += 1
        return out

    def category_counts(self) -> dict[str, int]:
        out: dict[str, int] = {}
        for f in self._items:
            out[str(f.category)] = out.get(str(f.category), 0) + 1
        return out

    # ---- serialisation ---------------------------------------------------

    def to_list(self) -> list[dict[str, Any]]:
        return [f.to_dict() for f in self._items]

    def write_json(self, path) -> None:
        payload = {
            "counts": self.counts(),
            "categories": self.category_counts(),
            "findings": self.to_list(),
        }
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)

    def render(self, minimum: Severity = Severity.INFO) -> str:
        return "\n\n".join(f.render() for f in self.by_severity(minimum))
