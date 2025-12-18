"""Visualization utilities for detection and reduction results."""

import cv2
import numpy as np
from typing import List, Dict, Tuple


class Visualizer:
    """Visualization utilities for detection and reduction results."""
    
    # Color map for different classes (BGR format)
    COLORS: Dict[str, Tuple[int, int, int]] = {
        'person': (0, 255, 0),       # Green
        'car': (255, 0, 0),          # Blue
        'truck': (0, 0, 255),        # Red
        'bicycle': (255, 255, 0),    # Cyan
        'motorcycle': (255, 0, 255), # Magenta
        'bus': (0, 255, 255),        # Yellow
        'traffic light': (128, 0, 128),
        'stop sign': (0, 128, 255),
        'default': (128, 128, 128)   # Gray
    }
    
    @classmethod
    def get_color(cls, class_name: str) -> Tuple[int, int, int]:
        """Get color for a class name."""
        return cls.COLORS.get(class_name, cls.COLORS['default'])
    
    @staticmethod
    def draw_detections(frame: np.ndarray, metadata: List[Dict],
                       show_labels: bool = True,
                       show_confidence: bool = True) -> np.ndarray:
        """
        Draw detection boxes on frame.
        
        Args:
            frame: Input image
            metadata: List of detection metadata dictionaries
            show_labels: Whether to show class labels
            show_confidence: Whether to show confidence scores
            
        Returns:
            Annotated frame
        """
        vis_frame = frame.copy()
        
        for obj in metadata:
            bbox = obj["bbox"]
            class_name = obj["class"]
            status = obj.get("status", "unknown")
            confidence = obj.get("confidence", 0)
            
            color = Visualizer.get_color(class_name)
            
            # Draw bounding box
            cv2.rectangle(vis_frame, 
                         (bbox[0], bbox[1]), 
                         (bbox[2], bbox[3]), 
                         color, 2)
            
            if show_labels:
                # Build label text
                label = f"{class_name}"
                if show_confidence:
                    label += f" {confidence:.2f}"
                label += f" ({status})"
                
                label_size, _ = cv2.getTextSize(
                    label, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1
                )
                
                # Draw label background
                cv2.rectangle(vis_frame,
                            (bbox[0], bbox[1] - label_size[1] - 10),
                            (bbox[0] + label_size[0], bbox[1]),
                            color, -1)
                
                # Draw label text
                cv2.putText(vis_frame, label, 
                           (bbox[0], bbox[1] - 5),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.5, 
                           (255, 255, 255), 1)
        
        return vis_frame
    
    @staticmethod
    def create_comparison(original: np.ndarray, 
                         processed: np.ndarray,
                         metadata: List[Dict],
                         title_left: str = "Original + Detections",
                         title_right: str = "After Reduction") -> np.ndarray:
        """
        Create side-by-side comparison image.
        
        Args:
            original: Original frame
            processed: Processed frame
            metadata: Detection metadata for annotations
            title_left: Title for left image
            title_right: Title for right image
            
        Returns:
            Combined comparison image
        """
        # Draw detections on original
        original_annotated = Visualizer.draw_detections(original.copy(), metadata)
        
        # Ensure same size
        h1, w1 = original_annotated.shape[:2]
        h2, w2 = processed.shape[:2]
        
        if h1 != h2 or w1 != w2:
            processed = cv2.resize(processed, (w1, h1))
        
        # Create side-by-side
        comparison = np.hstack([original_annotated, processed])
        
        # Add titles
        cv2.putText(comparison, title_left, (10, 30),
                   cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
        cv2.putText(comparison, title_right, (w1 + 10, 30),
                   cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
        
        return comparison
    
    @staticmethod
    def add_info_bar(frame: np.ndarray, info_text: str,
                    position: str = "bottom") -> np.ndarray:
        """
        Add information bar to frame.
        
        Args:
            frame: Input image
            info_text: Text to display
            position: "top" or "bottom"
            
        Returns:
            Frame with info bar
        """
        h, w = frame.shape[:2]
        
        if position == "bottom":
            y = h - 20
        else:
            y = 30
        
        cv2.putText(frame, info_text, (10, y),
                   cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
        
        return frame
    
    @staticmethod
    def create_grid(images: List[np.ndarray], 
                   cols: int = 2,
                   titles: List[str] = None) -> np.ndarray:
        """
        Create a grid of images.
        
        Args:
            images: List of images
            cols: Number of columns
            titles: Optional list of titles for each image
            
        Returns:
            Grid image
        """
        if not images:
            return np.zeros((100, 100, 3), dtype=np.uint8)
        
        # Ensure all images have same size
        h, w = images[0].shape[:2]
        resized = []
        for i, img in enumerate(images):
            if img.shape[:2] != (h, w):
                img = cv2.resize(img, (w, h))
            
            # Add title if provided
            if titles and i < len(titles):
                img = img.copy()
                cv2.putText(img, titles[i], (10, 25),
                           cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
            
            resized.append(img)
        
        # Pad to fill grid
        rows = (len(resized) + cols - 1) // cols
        while len(resized) < rows * cols:
            resized.append(np.zeros((h, w, 3), dtype=np.uint8))
        
        # Build grid
        grid_rows = []
        for i in range(rows):
            row_images = resized[i * cols:(i + 1) * cols]
            grid_rows.append(np.hstack(row_images))
        
        return np.vstack(grid_rows)