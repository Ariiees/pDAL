"""Dataset loader for image sequences."""

import cv2
from pathlib import Path
from typing import List, Dict, Optional


class ImageSequenceLoader:
    """Load and iterate through image sequence dataset."""
    
    def __init__(self, data_dir: str, extensions: Optional[List[str]] = None):
        """
        Initialize loader.
        
        Args:
            data_dir: Path to image directory (e.g., ~/dataset/0000)
            extensions: List of valid extensions (default: jpg, png)
        """
        self.data_dir = Path(data_dir).expanduser()
        self.extensions = extensions or ['.jpg', '.jpeg', '.png']
        
        # Get all image files sorted by name
        self.image_files = sorted([
            f for f in self.data_dir.iterdir()
            if f.suffix.lower() in self.extensions
        ])
        
        self.current_idx = 0
        print(f"Found {len(self.image_files)} images in {self.data_dir}")
    
    def __len__(self) -> int:
        return len(self.image_files)
    
    def __iter__(self):
        self.current_idx = 0
        return self
    
    def __next__(self) -> Dict:
        if self.current_idx >= len(self.image_files):
            raise StopIteration
        
        image_path = self.image_files[self.current_idx]
        self.current_idx += 1
        
        return self.load_frame(image_path)
    
    def load_frame(self, image_path: Path) -> Dict:
        """Load a single frame."""
        frame = cv2.imread(str(image_path))
        
        if frame is None:
            raise ValueError(f"Failed to load image: {image_path}")
        
        frame_id = image_path.stem
        
        return {
            "frame": frame,
            "frame_id": frame_id,
            "image_path": str(image_path)
        }
    
    def get_frame_by_index(self, idx: int) -> Dict:
        """Load frame by index."""
        if 0 <= idx < len(self.image_files):
            return self.load_frame(self.image_files[idx])
        else:
            raise IndexError(f"Index {idx} out of range (0-{len(self.image_files)-1})")
    
    def get_frame_by_id(self, frame_id: str) -> Dict:
        """Load frame by ID (filename without extension)."""
        for img_path in self.image_files:
            if img_path.stem == frame_id:
                return self.load_frame(img_path)
        raise FileNotFoundError(f"Frame {frame_id} not found")
    
    def reset(self):
        """Reset iterator to beginning."""
        self.current_idx = 0