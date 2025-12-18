"""Utility functions for the pipeline."""

import json
from pathlib import Path
from typing import Dict


def ensure_dir(path: str) -> Path:
    """Create directory if it doesn't exist."""
    p = Path(path)
    p.mkdir(parents=True, exist_ok=True)
    return p


def save_json(data: Dict, filepath: str, indent: int = 2):
    """Save dictionary to JSON file."""
    with open(filepath, 'w') as f:
        json.dump(data, f, indent=indent)


def load_json(filepath: str) -> Dict:
    """Load dictionary from JSON file."""
    with open(filepath, 'r') as f:
        return json.load(f)


def save_policy_to_json(policy_id: str, rules: Dict, 
                       filepath: str, description: str = ""):
    """Save a policy configuration to JSON file."""
    policy_data = {
        "policy_id": policy_id,
        "description": description,
        "reduction_rules": rules
    }
    save_json(policy_data, filepath)


def format_statistics(stats: Dict) -> str:
    """Format statistics dictionary as readable string."""
    timing = stats.get("timing", {})
    
    lines = [
        "=" * 60,
        "Processing Statistics",
        "=" * 60,
        f"Frames processed:      {stats.get('frames_processed', 0)}",
        f"Total detections:      {stats.get('total_detections', 0)}",
        f"Total anonymized:      {stats.get('total_anonymized', 0)}",
        f"Total removed:         {stats.get('total_removed', 0)}",
        "",
        "Timing Breakdown:",
        f"  Avg detection:       {timing.get('avg_detection_ms', 0):.2f} ms",
        f"  Avg anonymization:   {timing.get('avg_anonymization_ms', 0):.2f} ms",
        f"  Avg total:           {timing.get('avg_total_ms', 0):.2f} ms",
        "",
        f"Average FPS:           {stats.get('fps', 0):.2f}",
        "=" * 60
    ]
    return "\n".join(lines)


def print_progress(current: int, total: int, frame_id: str, 
                  result: Dict, width: int = 40):
    """Print progress bar with frame info."""
    percent = current / total
    filled = int(width * percent)
    bar = "█" * filled + "░" * (width - filled)
    
    timing = result.get("timing", {})
    total_ms = timing.get("total_ms", 0)
    
    info = (f"\r[{bar}] {current}/{total} | "
            f"Frame: {frame_id} | "
            f"Det: {result['detections']} | "
            f"Anon: {result['anonymized']} | "
            f"Time: {total_ms:.1f}ms")
    
    print(info, end="", flush=True)
    
    if current == total:
        print()