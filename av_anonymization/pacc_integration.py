# pacc_integration.py
"""Integration module for Detection-Reduction Pipeline with VPI/PaCC"""

import base64
import io
import cv2
import numpy as np
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
        output_dir: str = "./pacc_output"
    ):
        """
        Initialize the detection pipeline.
        
        Args:
            model_path: Path to YOLO model
            use_openvino: Whether to use OpenVINO optimization
            output_dir: Directory to save processed images
        """
        self.pipeline = DetectionReductionPipeline(model_path, use_openvino=use_openvino)
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Create subdirectories
        self.frames_dir = self.output_dir / "frames"
        self.frames_dir.mkdir(exist_ok=True)
        
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
            Anonymized detection results in VPI format with processed images
        """
        instruction = payload.get("instruction", {})
        avs_result = payload.get("avs_result", {})
        access_context = payload.get("access_context", {})
        
        # Determine which policy to use based on access context
        policy_id = self._select_policy(access_context, instruction)
        
        # Determine output format preferences
        output_format = instruction.get("output_format", {})
        include_images = output_format.get("include_images", True)
        image_format = output_format.get("image_format", "jpg")  # jpg or png
        image_encoding = output_format.get("image_encoding", "file")  # file or base64
        
        # Process each encrypted record from AVS
        frames_output = []
        for record in avs_result.get("records", []):
            frame_result = self._process_encrypted_frame(
                record, 
                instruction, 
                policy_id,
                include_images=include_images,
                image_format=image_format,
                image_encoding=image_encoding
            )
            if frame_result:
                frames_output.append(frame_result)
        
        return {
            "frames": frames_output,
            "policy_applied": policy_id,
            "processing_metadata": {
                "total_frames": len(frames_output),
                "avg_fps": self.pipeline.get_statistics().get("fps", 0),
                "output_directory": str(self.frames_dir)
            }
        }
    
    def _select_policy(self, access_context: JsonDict, instruction: JsonDict) -> str:
        """
        Select appropriate reduction policy based on access permissions.
        
        Priority:
        1. Explicit policy in access_context
        2. Derive from approved_objects
        3. Default to most restrictive
        """
        # Check if policy is explicitly specified
        if "policy_id" in access_context:
            policy_map = {
                "demo_policy_v1": "traffic_monitoring",
                "full_access": "minimal_reduction",
                "research_policy": "partial_anonymization",
            }
            return policy_map.get(access_context["policy_id"], "full_anonymization")
        
        # Derive from approved objects
        approved = access_context.get("approved_objects", [])
        if not approved:
            return "full_anonymization"
        
        # Less restrictive if more object types are approved
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
    ) -> JsonDict:
        """
        Process a single encrypted frame from AVS.
        
        In production, this would:
        1. Decrypt the frame inside the confidential enclave
        2. Run detection/reduction
        3. Save processed image
        4. Return only approved, anonymized results
        
        Args:
            record: AVS record with encrypted frame reference
            instruction: Processing instruction
            policy_id: Reduction policy to apply
            include_images: Whether to include processed images in output
            image_format: 'jpg' or 'png'
            image_encoding: 'file' (save to disk) or 'base64' (embed in response)
        """
        # Simulate decryption (in production, this happens in secure enclave)
        frame = self._decrypt_frame(record.get("ciphertext_ref", ""))
        
        if frame is None:
            return None
        
        # Run detection + reduction pipeline
        result = self.pipeline.process(frame, policy_id)
        
        # Filter results based on object queries
        filtered_objects = self._filter_by_queries(
            result["metadata"],
            instruction.get("object_queries", [])
        )
        
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
            }
        }
        
        # Add processed image to output
        if include_images:
            image_data = self._save_processed_frame(
                result["frame"],
                record.get("record_id", "unknown"),
                image_format=image_format,
                encoding=image_encoding
            )
            frame_output["processed_image"] = image_data
        
        return frame_output
    
    def _save_processed_frame(
        self,
        frame: np.ndarray,
        record_id: str,
        image_format: str = "jpg",
        encoding: str = "file"
    ) -> JsonDict:
        """
        Save processed frame to disk or encode as base64.
        
        Args:
            frame: Processed frame (numpy array)
            record_id: Unique identifier for the frame
            image_format: 'jpg' or 'png'
            encoding: 'file' or 'base64'
            
        Returns:
            Dictionary with image location/data and metadata
        """
        # Determine file extension
        ext = "jpg" if image_format == "jpg" else "png"
        filename = f"{record_id}_processed.{ext}"
        filepath = self.frames_dir / filename
        
        # Set encoding parameters
        if image_format == "jpg":
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, 95]
        else:  # png
            encode_params = [cv2.IMWRITE_PNG_COMPRESSION, 3]
        
        if encoding == "base64":
            # Encode to base64 for embedding in JSON response
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
        
        else:  # file encoding (default)
            # Save to disk
            cv2.imwrite(str(filepath), frame, encode_params)
            
            # Get file size
            file_size = filepath.stat().st_size
            
            return {
                "format": image_format,
                "encoding": "file",
                "path": str(filepath),
                "filename": filename,
                "url": f"file://{filepath.absolute()}",  # Can be replaced with HTTP URL in production
                "size_bytes": file_size,
                "dimensions": {
                    "width": frame.shape[1],
                    "height": frame.shape[0]
                }
            }
    
    def _decrypt_frame(self, ciphertext_ref: str) -> Optional[np.ndarray]:
        """
        Simulate frame decryption from AVS.
        
        In production PaCC:
        - This runs inside AMD SEV-SNP or Intel TDX enclave
        - Decryption keys never leave secure memory
        - Frame data is never exposed to host OS
        
        For demo: Load from local path or generate test image
        """
        # Option 1: If you have actual image files
        # Extract frame ID from reference
        if "blob" in ciphertext_ref:
            frame_id = ciphertext_ref.split("/")[-1]
            # Try to load from a test directory
            test_path = Path(f"~/dataset/0000/{frame_id}.jpg").expanduser()
            print(f"test_path: {test_path}")
            if test_path.exists():
                print("Exist")
                return cv2.imread(str(test_path))
        
        # Option 2: Generate synthetic test frame
        return self._generate_test_frame()
    
    def _generate_test_frame(self) -> np.ndarray:
        """Generate a synthetic test frame for demo purposes."""
        # Create a simple street scene simulation
        frame = np.ones((720, 1280, 3), dtype=np.uint8) * 128
        
        # Add some geometric shapes to simulate objects
        # Car-like rectangle
        cv2.rectangle(frame, (200, 400), (400, 550), (100, 100, 200), -1)
        # Person-like ellipse
        cv2.ellipse(frame, (800, 450), (50, 100), 0, 0, 360, (150, 100, 100), -1)
        
        return frame
    
    def _filter_by_queries(
        self, metadata: List[Dict], object_queries: List[JsonDict]
    ) -> List[JsonDict]:
        """
        Filter detection results based on VPI object queries.
        
        Converts internal detection format to VPI output format.
        """
        if not object_queries:
            # No specific queries, return all (subject to policy)
            return self._format_all_objects(metadata)
        
        filtered = []
        
        for query in object_queries:
            query_id = query.get("query_id")
            object_type = query.get("object_type")
            attributes = query.get("attributes", {})
            
            # Find matching detections
            matched = self._match_query(metadata, object_type, attributes)
            
            # Format matches with query_id
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
        # Map VPI object type to internal class
        internal_class = self.object_type_mapping.get(object_type, object_type)
        
        matched = []
        for obj in metadata:
            # Check class match
            if obj.get("class") != internal_class:
                continue
            
            # Check confidence threshold
            conf_threshold = attributes.get("confidence_threshold", 0.0)
            if obj.get("confidence", 0) < conf_threshold:
                continue
            
            # Additional attribute filtering could go here
            # (age_range, state, etc. would require specialized models)
            
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
        """
        Ensure bbox is in normalized coordinates [x1, y1, x2, y2].
        
        VPI spec expects normalized coordinates (0-1 range).
        """
        if not bbox or len(bbox) != 4:
            return [0.0, 0.0, 0.0, 0.0]
        
        # If already normalized, return as-is
        if all(0 <= coord <= 1 for coord in bbox):
            return bbox
        
        # Otherwise, would need image dimensions to normalize
        # For now, assume they're already normalized
        return bbox


# Update the VPI handler to use real PaCC service
def create_pacc_anonymize_handler(
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