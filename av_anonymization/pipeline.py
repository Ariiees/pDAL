"""Detection and reduction pipeline."""

import cv2
import numpy as np
import time
import json
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from ultralytics import YOLO

from models import (
    Detection, 
    ReductionRule, 
    ReductionAction, 
    AnonymizeMethod
)


class DetectionReductionPipeline:
    """Combined detection and configurable information reduction."""
    
    # Classes relevant to autonomous driving
    AV_CLASSES = {
        'person', 'bicycle', 'car', 'motorcycle', 'bus', 'truck',
        'traffic light', 'stop sign', 'parking meter'
    }
    
    def __init__(self, model_path: str = "yolov8n.pt", use_openvino: bool = False):
        """
        Initialize the pipeline.
        
        Args:
            model_path: Path to YOLO model
            use_openvino: Whether to use OpenVINO optimization for CPU
        """
        print(f"Loading model: {model_path}")
        self.model = YOLO(model_path)
        
        if use_openvino:
            self._setup_openvino()
        
        self.class_names = self.model.names
        
        # Default rule - remove everything not explicitly allowed
        self.default_rule = ReductionRule(
            action=ReductionAction.REMOVE,
            output_metadata=False
        )
        
        # Registered policies
        self.policies: Dict[str, Dict[str, ReductionRule]] = {}
        
        # Statistics
        self.stats = {
            "frames_processed": 0,
            "total_detections": 0,
            "total_anonymized": 0,
            "total_removed": 0,
            "avg_detection_time": 0,
            "avg_anonymization_time": 0,
            "avg_total_time": 0
        }
    
    def _setup_openvino(self):
        """Setup OpenVINO for CPU optimization."""
        try:
            openvino_path = Path("yolov8n_openvino_model")
            if not openvino_path.exists():
                print("Exporting model to OpenVINO format...")
                self.model.export(format="openvino")
            self.model = YOLO("yolov8n_openvino_model")
            print("OpenVINO model loaded successfully")
        except Exception as e:
            print(f"OpenVINO setup failed, using default model: {e}")
    
    def register_policy(self, policy_id: str, rules: Dict):
        """
        Register a reduction policy.
        
        Args:
            policy_id: Unique identifier for the policy
            rules: Dictionary mapping class names to reduction rules
        """
        parsed_rules = {}
        for class_name, rule_config in rules.items():
            parsed_rules[class_name] = ReductionRule(
                action=ReductionAction(rule_config.get("action", "remove")),
                method=AnonymizeMethod(rule_config["method"]) if rule_config.get("method") else None,
                output_metadata=rule_config.get("output_metadata", True)
            )
        self.policies[policy_id] = parsed_rules
        print(f"Registered policy: {policy_id}")
    
    def register_policy_from_json(self, json_path: str):
        """Load and register policy from JSON file."""
        with open(json_path, 'r') as f:
            policy_data = json.load(f)
        
        policy_id = policy_data.get("policy_id", "default")
        rules = policy_data.get("reduction_rules", {})
        self.register_policy(policy_id, rules)
    
    def detect(self, frame: np.ndarray, conf_threshold: float = 0.25,
               filter_av_classes: bool = True) -> Tuple[List[Detection], float]:
        """
        Run object detection on frame.
        
        Args:
            frame: Input image (BGR format)
            conf_threshold: Confidence threshold for detections
            filter_av_classes: Whether to filter to AV-relevant classes only
            
        Returns:
            Tuple of (List of Detection objects, inference time in seconds)
        """
        start_time = time.time()
        results = self.model(frame, verbose=False, conf=conf_threshold)[0]
        detection_time = time.time() - start_time
        
        detections = []
        for box in results.boxes:
            class_id = int(box.cls)
            class_name = self.class_names[class_id]
            
            if not filter_av_classes or class_name in self.AV_CLASSES:
                detections.append(Detection(
                    class_name=class_name,
                    confidence=float(box.conf),
                    bbox=tuple(map(int, box.xyxy[0].tolist())),
                    class_id=class_id
                ))
        
        return detections, detection_time
    
    def _update_time_stats(self, detection_time: float, anonymization_time: float):
        """
        Update running average of processing times.
        
        Args:
            detection_time: Time spent on detection (seconds)
            anonymization_time: Time spent on anonymization (seconds)
        """
        n = self.stats["frames_processed"]
        total_time = detection_time + anonymization_time
        
        if n == 0:
            self.stats["avg_detection_time"] = detection_time
            self.stats["avg_anonymization_time"] = anonymization_time
            self.stats["avg_total_time"] = total_time
        else:
            # Running average formula: new_avg = (old_avg * n + new_value) / (n + 1)
            self.stats["avg_detection_time"] = (
                self.stats["avg_detection_time"] * n + detection_time
            ) / (n + 1)
            self.stats["avg_anonymization_time"] = (
                self.stats["avg_anonymization_time"] * n + anonymization_time
            ) / (n + 1)
            self.stats["avg_total_time"] = (
                self.stats["avg_total_time"] * n + total_time
            ) / (n + 1)
    
    def apply_anonymization(self, frame: np.ndarray, 
                           bbox: Tuple[int, int, int, int],
                           method: AnonymizeMethod) -> np.ndarray:
        """
        Apply anonymization method to a region.
        
        Args:
            frame: Input image
            bbox: Bounding box (x1, y1, x2, y2)
            method: Anonymization method to apply
            
        Returns:
            Modified frame
        """
        x1, y1, x2, y2 = bbox
        
        # Ensure valid coordinates
        h, w = frame.shape[:2]
        x1, y1 = max(0, x1), max(0, y1)
        x2, y2 = min(w, x2), min(h, y2)
        
        if x2 <= x1 or y2 <= y1:
            return frame
        
        roi = frame[y1:y2, x1:x2].copy()
        
        if method == AnonymizeMethod.BLUR:
            kernel_size = max(15, (x2 - x1) // 3)
            kernel_size = kernel_size if kernel_size % 2 == 1 else kernel_size + 1
            roi = cv2.GaussianBlur(roi, (kernel_size, kernel_size), 30)
            
        elif method == AnonymizeMethod.PIXELATE:
            roi_h, roi_w = roi.shape[:2]
            factor = 10
            small = cv2.resize(roi, (max(1, roi_w // factor), max(1, roi_h // factor)))
            roi = cv2.resize(small, (roi_w, roi_h), interpolation=cv2.INTER_NEAREST)
            
        elif method == AnonymizeMethod.BLACKOUT:
            roi = np.zeros_like(roi)
            
        elif method == AnonymizeMethod.MASK:
            roi[:] = (128, 128, 128)
            
        elif method == AnonymizeMethod.SILHOUETTE:
            gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
            _, mask = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
            roi = cv2.merge([mask, mask, mask])
        
        frame[y1:y2, x1:x2] = roi
        return frame
    
    def process(self, frame: np.ndarray, policy_id: str,
                conf_threshold: float = 0.25) -> Dict:
        """
        Process a frame through detection and reduction pipeline.
        
        Args:
            frame: Input image (BGR format)
            policy_id: ID of the policy to apply
            conf_threshold: Detection confidence threshold
            
        Returns:
            Dictionary containing processed frame, metadata, and timing info
        """
        # Step 1: Detect all objects (timed internally)
        detections, detection_time = self.detect(frame, conf_threshold)
        
        # Step 2: Get policy
        policy = self.policies.get(policy_id, {})
        
        # Step 3: Apply reduction based on policy (timed)
        anon_start_time = time.time()
        
        output_frame = frame.copy()
        output_metadata = []
        anonymized_count = 0
        removed_count = 0
        
        for det in detections:
            rule = policy.get(det.class_name, self.default_rule)
            
            if rule.action == ReductionAction.KEEP:
                if rule.output_metadata:
                    output_metadata.append({
                        "class": det.class_name,
                        "confidence": round(det.confidence, 3),
                        "bbox": det.bbox,
                        "status": "kept"
                    })
                    
            elif rule.action == ReductionAction.ANONYMIZE:
                output_frame = self.apply_anonymization(
                    output_frame, det.bbox, rule.method
                )
                anonymized_count += 1
                
                if rule.output_metadata:
                    output_metadata.append({
                        "class": det.class_name,
                        "confidence": round(det.confidence, 3),
                        "bbox": det.bbox,
                        "status": "anonymized",
                        "method": rule.method.value
                    })
                    
            elif rule.action == ReductionAction.REMOVE:
                output_frame = self.apply_anonymization(
                    output_frame, det.bbox, AnonymizeMethod.BLACKOUT
                )
                removed_count += 1
        
        anonymization_time = time.time() - anon_start_time
        
        # Update timing statistics
        self._update_time_stats(detection_time, anonymization_time)
        
        # Update count statistics
        self.stats["frames_processed"] += 1
        self.stats["total_detections"] += len(detections)
        self.stats["total_anonymized"] += anonymized_count
        self.stats["total_removed"] += removed_count
        
        return {
            "frame": output_frame,
            "metadata": output_metadata,
            "policy_id": policy_id,
            "detections": len(detections),
            "anonymized": anonymized_count,
            "removed": removed_count,
            "kept": len(detections) - anonymized_count - removed_count,
            "timing": {
                "detection_ms": round(detection_time * 1000, 2),
                "anonymization_ms": round(anonymization_time * 1000, 2),
                "total_ms": round((detection_time + anonymization_time) * 1000, 2)
            }
        }
    
    def get_statistics(self) -> Dict:
        """Get processing statistics including detailed timing."""
        fps = 0
        if self.stats["avg_total_time"] > 0:
            fps = 1.0 / self.stats["avg_total_time"]
        
        return {
            "frames_processed": self.stats["frames_processed"],
            "total_detections": self.stats["total_detections"],
            "total_anonymized": self.stats["total_anonymized"],
            "total_removed": self.stats["total_removed"],
            "timing": {
                "avg_detection_ms": round(self.stats["avg_detection_time"] * 1000, 2),
                "avg_anonymization_ms": round(self.stats["avg_anonymization_time"] * 1000, 2),
                "avg_total_ms": round(self.stats["avg_total_time"] * 1000, 2),
            },
            "fps": round(fps, 2)
        }
    
    def reset_statistics(self):
        """Reset all statistics."""
        self.stats = {
            "frames_processed": 0,
            "total_detections": 0,
            "total_anonymized": 0,
            "total_removed": 0,
            "avg_detection_time": 0,
            "avg_anonymization_time": 0,
            "avg_total_time": 0
        }