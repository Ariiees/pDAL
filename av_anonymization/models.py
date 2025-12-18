"""Data models and enums for the detection-reduction pipeline."""

from dataclasses import dataclass
from typing import Tuple, Optional
from enum import Enum


class ReductionAction(Enum):
    """Actions that can be taken on detected objects."""
    KEEP = "keep"
    ANONYMIZE = "anonymize"
    REMOVE = "remove"


class AnonymizeMethod(Enum):
    """Methods for anonymizing detected objects."""
    BLUR = "blur"
    PIXELATE = "pixelate"
    BLACKOUT = "blackout"
    MASK = "mask"
    SILHOUETTE = "silhouette"


@dataclass
class Detection:
    """Represents a single detected object."""
    class_name: str
    confidence: float
    bbox: Tuple[int, int, int, int]  # (x1, y1, x2, y2)
    class_id: int


@dataclass
class ReductionRule:
    """Rule for how to handle a specific class of objects."""
    action: ReductionAction
    method: Optional[AnonymizeMethod] = None
    output_metadata: bool = True


@dataclass
class ProcessingResult:
    """Result of processing a single frame."""
    frame: any  # np.ndarray
    metadata: list
    policy_id: str
    detections: int
    anonymized: int
    removed: int
    kept: int