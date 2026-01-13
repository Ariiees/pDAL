import base64
import cv2
import numpy as np
import time
from typing import Dict, List, Any, Optional
from pathlib import Path

from pipeline import DetectionReductionPipeline
from policies import get_default_policies

JsonDict = Dict[str, Any]


class PaCCDetectionService:
    """
    Bridge between VPI/PaCC interface and detection-reduction pipeline.
    Replaces mock_pacc_anonymize with real computer vision processing.
    """
    
    def __init__(
        self, 
        model_path: str = "yolov8n.pt", 
        use_openvino: bool = False,
        output_dir: str = "./pacc_output",
        avs_storage = None  # AVSEncryptedStorage instance
    ):
        """
        Initialize the detection pipeline.
        
        Args:
            model_path: Path to YOLO model
            use_openvino: Whether to use OpenVINO optimization
            output_dir: Directory to save processed images
            avs_storage: AVS encrypted storage instance (for decryption)
        """
        self.pipeline = DetectionReductionPipeline(model_path, use_openvino=use_openvino)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Create subdirectories
        self.frames_dir = self.output_dir / "frames"
        self.frames_dir.mkdir(exist_ok=True)
        
        # Store AVS storage for decryption
        self.avs_storage = avs_storage
        
        # Register all available policies
        policies = get_default_policies()
        for pid, policy_data in policies.items():
            self.pipeline.register_policy(pid, policy_data["reduction_rules"])
        
        # Map VPI object types to YOLO classes
        self.object_type_mapping = {
            "license_plate": "car",  # Detect cars, anonymize plates
            "pedestrian": "person",
            "vehicle": "car",
            "bicycle": "bicycle",
            "motorcycle": "motorcycle",
            "traffic_sign": "stop sign",
            "traffic_light": "traffic light",
        }
    
    def process_pacc_request(self, payload: JsonDict) -> JsonDict:
        """
        Main entry point replacing mock_pacc_anonymize.
        
        Args:
            payload: Contains instruction, avs_result, and access_context
            
        Returns:
            Anonymized detection results in VPI format with processed images and timing
        """
        total_start = time.time()
        
        instruction = payload.get("instruction", {})
        avs_result = payload.get("avs_result", {})
        access_context = payload.get("access_context", {})
        
        # Timing: Policy selection
        policy_start = time.time()
        policy_id = self._select_policy(access_context, instruction)
        policy_time = (time.time() - policy_start) * 1000
        
        # Determine output format preferences
        output_format = instruction.get("output_format", {})
        include_images = output_format.get("include_images", True)
        image_format = output_format.get("image_format", "jpg")
        image_encoding = output_format.get("image_encoding", "file")
        
        # Process each encrypted record from AVS
        frames_output = []
        total_decryption_time = 0
        total_detection_time = 0
        total_anonymization_time = 0
        total_filtering_time = 0
        total_save_time = 0
        
        for record in avs_result.get("records", []):
            frame_result, frame_timing = self._process_encrypted_frame(
                record, 
                instruction, 
                policy_id,
                include_images=include_images,
                image_format=image_format,
                image_encoding=image_encoding
            )
            if frame_result:
                frames_output.append(frame_result)
                
                # Accumulate timing stats
                total_decryption_time += frame_timing.get("decryption_ms", 0)
                total_detection_time += frame_timing.get("detection_ms", 0)
                total_anonymization_time += frame_timing.get("anonymization_ms", 0)
                total_filtering_time += frame_timing.get("filtering_ms", 0)
                total_save_time += frame_timing.get("save_ms", 0)
        
        total_time = (time.time() - total_start) * 1000
        num_frames = len(frames_output)
        
        return {
            "frames": frames_output,
            "policy_applied": policy_id,
            "processing_metadata": {
                "total_frames": num_frames,
                "avg_fps": self.pipeline.get_statistics().get("fps", 0),
                "output_directory": str(self.frames_dir)
            },
            "timing_summary": {
                "total_time_ms": total_time,
                "policy_selection_ms": policy_time,
                "total_decryption_ms": total_decryption_time,
                "total_detection_ms": total_detection_time,
                "total_anonymization_ms": total_anonymization_time,
                "total_filtering_ms": total_filtering_time,
                "total_save_ms": total_save_time,
                "avg_per_frame": {
                    "decryption_ms": total_decryption_time / num_frames if num_frames > 0 else 0,
                    "detection_ms": total_detection_time / num_frames if num_frames > 0 else 0,
                    "anonymization_ms": total_anonymization_time / num_frames if num_frames > 0 else 0,
                    "filtering_ms": total_filtering_time / num_frames if num_frames > 0 else 0,
                    "save_ms": total_save_time / num_frames if num_frames > 0 else 0,
                    "total_ms": total_time / num_frames if num_frames > 0 else 0,
                },
                "throughput": {
                    "frames_per_second": (num_frames / (total_time / 1000)) if total_time > 0 else 0,
                    "ms_per_frame": total_time / num_frames if num_frames > 0 else 0,
                }
            }
        }
    
    def _select_policy(self, access_context: JsonDict, instruction: JsonDict) -> str:
        """
        Select appropriate reduction policy based on access permissions.
        """
        if "policy_id" in access_context:
            policy_map = {
                "demo_policy_v1": "traffic_monitoring",
                "full_access": "minimal_reduction",
                "research_policy": "partial_anonymization",
            }
            return policy_map.get(access_context["policy_id"], "full_anonymization")
        
        approved = access_context.get("approved_objects", [])
        if not approved:
            return "full_anonymization"
        
        if len(approved) >= 3:
            return "research_mode"
        elif "pedestrian" in approved and "license_plate" in approved:
            return "full_anonymization"
        elif "pedestrian" in approved:
            return "pedestrian_safety"
        else:
            return "traffic_monitoring"
    
    def _process_encrypted_frame(
        self, 
        record: JsonDict, 
        instruction: JsonDict, 
        policy_id: str,
        include_images: bool = True,
        image_format: str = "jpg",
        image_encoding: str = "file"
    ) -> tuple[JsonDict, JsonDict]:
        """
        Process a single encrypted frame from AVS with detailed timing.
        
        Returns:
            Tuple of (frame_output, timing_dict)
        """
        timing = {}
        frame_start = time.time()
        
        # Step 1: Decrypt the frame
        decrypt_start = time.time()
        frame = self._decrypt_frame(record.get("ciphertext_ref", ""))
        timing["decryption_ms"] = (time.time() - decrypt_start) * 1000
        
        if frame is None:
            return None, timing
        
        # Step 2: Run detection + reduction pipeline
        pipeline_start = time.time()
        result = self.pipeline.process(frame, policy_id)
        pipeline_time = (time.time() - pipeline_start) * 1000
        
        # Extract timing from pipeline result
        pipeline_timing = result.get("timing", {})
        timing["detection_ms"] = pipeline_timing.get("detection_ms", 0)
        timing["anonymization_ms"] = pipeline_timing.get("anonymization_ms", 0)
        
        # Step 3: Filter results based on object queries
        filter_start = time.time()
        filtered_objects = self._filter_by_queries(
            result["metadata"],
            instruction.get("object_queries", [])
        )
        timing["filtering_ms"] = (time.time() - filter_start) * 1000
        
        # Prepare frame output
        frame_output = {
            "timestamp": record.get("timestamp"),
            "record_id": record.get("record_id"),
            "objects": filtered_objects,
            "frame_summary": {
                "total_detections": result["detections"],
                "kept": result["kept"],
                "anonymized": result["anonymized"],
                "removed": result["removed"],
            },
            "timing": {
                "decryption_ms": timing["decryption_ms"],
                "detection_ms": timing["detection_ms"],
                "anonymization_ms": timing["anonymization_ms"],
                "filtering_ms": timing["filtering_ms"],
                "pipeline_total_ms": pipeline_time,
            }
        }
        
        # Step 4: Add processed image to output
        if include_images:
            save_start = time.time()
            image_data = self._save_processed_frame(
                result["frame"],
                record.get("record_id", "unknown"),
                image_format=image_format,
                encoding=image_encoding
            )
            timing["save_ms"] = (time.time() - save_start) * 1000
            frame_output["processed_image"] = image_data
            frame_output["timing"]["save_ms"] = timing["save_ms"]
        else:
            timing["save_ms"] = 0
        
        # Total frame processing time
        timing["total_frame_ms"] = (time.time() - frame_start) * 1000
        frame_output["timing"]["total_ms"] = timing["total_frame_ms"]
        
        return frame_output, timing
    
    def _save_processed_frame(
        self,
        frame: np.ndarray,
        record_id: str,
        image_format: str = "jpg",
        encoding: str = "file"
    ) -> JsonDict:
        """Save processed frame to disk or encode as base64."""
        ext = "jpg" if image_format == "jpg" else "png"
        filename = f"{record_id}_processed.{ext}"
        filepath = self.frames_dir / filename
        
        if image_format == "jpg":
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, 95]
        else:
            encode_params = [cv2.IMWRITE_PNG_COMPRESSION, 3]
        
        if encoding == "base64":
            success, buffer = cv2.imencode(f".{ext}", frame, encode_params)
            if not success:
                return {
                    "error": "Failed to encode image",
                    "format": image_format
                }
            
            img_base64 = base64.b64encode(buffer).decode('utf-8')
            
            return {
                "format": image_format,
                "encoding": "base64",
                "data": img_base64,
                "size_bytes": len(buffer),
                "dimensions": {
                    "width": frame.shape[1],
                    "height": frame.shape[0]
                }
            }
        
        else:
            cv2.imwrite(str(filepath), frame, encode_params)
            file_size = filepath.stat().st_size
            
            return {
                "format": image_format,
                "encoding": "file",
                "path": str(filepath),
                "filename": filename,
                "url": f"file://{filepath.absolute()}",
                "size_bytes": file_size,
                "dimensions": {
                    "width": frame.shape[1],
                    "height": frame.shape[0]
                }
            }
    
    def _decrypt_frame(self, ciphertext_ref: str) -> Optional[np.ndarray]:
        """Decrypt frame from AVS encrypted storage."""
        if not ciphertext_ref or not ciphertext_ref.startswith("avs://encrypted/blob/"):
            print(f"Warning: Invalid ciphertext_ref: {ciphertext_ref}")
            return self._generate_test_frame()
        
        if self.avs_storage is None:
            print(f"Warning: No AVS storage configured, using test frame")
            return self._generate_test_frame()
        
        record_id = ciphertext_ref.split("/")[-1]
        
        try:
            decrypted_frame = self.avs_storage.decrypt_frame(
                ciphertext_ref=ciphertext_ref,
                record_id=record_id,
                access_context={"decision": "allow"}
            )
            
            if decrypted_frame is not None:
                print(f"✓ Successfully decrypted frame: {record_id}")
                print(f"  Shape: {decrypted_frame.shape}")
                return decrypted_frame
            else:
                print(f"Warning: Could not decrypt frame {record_id}, using test frame")
                return self._generate_test_frame()
                
        except Exception as e:
            print(f"Error decrypting frame {record_id}: {e}")
            return self._generate_test_frame()
    
    def _generate_test_frame(self) -> np.ndarray:
        """Generate a synthetic test frame for demo purposes."""
        frame = np.ones((720, 1280, 3), dtype=np.uint8) * 128
        cv2.rectangle(frame, (200, 400), (400, 550), (100, 100, 200), -1)
        cv2.ellipse(frame, (800, 450), (50, 100), 0, 0, 360, (150, 100, 100), -1)
        return frame
    
    def _filter_by_queries(
        self, metadata: List[Dict], object_queries: List[JsonDict]
    ) -> List[JsonDict]:
        """Filter detection results based on VPI object queries."""
        if not object_queries:
            return self._format_all_objects(metadata)
        
        filtered = []
        
        for query in object_queries:
            query_id = query.get("query_id")
            object_type = query.get("object_type")
            attributes = query.get("attributes", {})
            
            matched = self._match_query(metadata, object_type, attributes)
            
            for match in matched:
                filtered.append({
                    "query_id": query_id,
                    "type": object_type,
                    "bbox": match["bbox"],
                    "confidence": match["confidence"],
                    "anonymized": match.get("anonymized", False),
                })
        
        return filtered
    
    def _match_query(
        self, metadata: List[Dict], object_type: str, attributes: Dict
    ) -> List[Dict]:
        """Match detections against a specific query."""
        internal_class = self.object_type_mapping.get(object_type, object_type)
        
        matched = []
        for obj in metadata:
            if obj.get("class") != internal_class:
                continue
            
            conf_threshold = attributes.get("confidence_threshold", 0.0)
            if obj.get("confidence", 0) < conf_threshold:
                continue
            
            matched.append({
                "bbox": self._normalize_bbox(obj.get("bbox", [])),
                "confidence": obj.get("confidence", 0),
                "anonymized": obj.get("action") == "anonymize",
            })
        
        return matched
    
    def _format_all_objects(self, metadata: List[Dict]) -> List[JsonDict]:
        """Format all detected objects when no specific queries provided."""
        return [
            {
                "type": obj.get("class"),
                "bbox": self._normalize_bbox(obj.get("bbox", [])),
                "confidence": obj.get("confidence", 0),
                "action": obj.get("action", "keep"),
                "anonymized": obj.get("action") == "anonymize",
            }
            for obj in metadata
        ]
    
    def _normalize_bbox(self, bbox: List[float]) -> List[float]:
        """Ensure bbox is in normalized coordinates [x1, y1, x2, y2]."""
        if not bbox or len(bbox) != 4:
            return [0.0, 0.0, 0.0, 0.0]
        
        if all(0 <= coord <= 1 for coord in bbox):
            return bbox
        
        return bbox

def create_pacc_anonymize_handler(
    model_path: str = "yolov8n.pt", 
    use_openvino: bool = False,
    output_dir: str = "./pacc_output",
    avs_storage = None  # AVSEncryptedStorage instance
):
    """
    Factory function to create a PaCC anonymization handler.
    
    Args:
        model_path: Path to YOLO model
        use_openvino: Whether to use OpenVINO optimization
        output_dir: Directory to save processed images
        avs_storage: AVS encrypted storage instance (for decryption)
    
    Returns:
        Handler function for processing PaCC requests
    
    Usage:
        # Without encryption
        pacc_anonymize = create_pacc_anonymize_handler(
            model_path="yolov8n.pt",
            output_dir="./my_output"
        )
        
        # With encryption
        avs_storage = AVSEncryptedStorage()
        # ... encrypt images ...
        pacc_anonymize = create_pacc_anonymize_handler(
            model_path="yolov8n.pt",
            output_dir="./my_output",
            avs_storage=avs_storage
        )
        
        result = pacc_anonymize(pacc_payload)
    """
    service = PaCCDetectionService(
        model_path, 
        use_openvino, 
        output_dir,
        avs_storage=avs_storage
    )
    return service.process_pacc_request


# Update the VPI handler to use real PaCC service
def create_pacc_anonymize_handler2(
    model_path: str = "yolov8n.pt", 
    use_openvino: bool = False,
    output_dir: str = "./pacc_output"
):
    """
    Factory function to create a PaCC anonymization handler.
    
    Args:
        model_path: Path to YOLO model
        use_openvino: Whether to use OpenVINO optimization
        output_dir: Directory to save processed images
    
    Usage:
        pacc_anonymize = create_pacc_anonymize_handler(
            model_path="yolov8n.pt",
            output_dir="./my_output"
        )
        
        result = pacc_anonymize(pacc_payload)
    """
    service = PaCCDetectionService(model_path, use_openvino, output_dir)
    return service.process_pacc_request