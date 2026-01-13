# encryption_utils.py
"""AES encryption/decryption utilities for image data."""

from Crypto.Cipher import AES
from Crypto.Random import get_random_bytes
from Crypto.Util.Padding import pad, unpad
import base64
import json
from typing import Tuple, Dict, Optional
import numpy as np
import cv2


class ImageEncryption:
    """Handles AES encryption/decryption of image data."""
    
    def __init__(self, key_size: int = 256):
        """
        Initialize encryption handler.
        
        Args:
            key_size: AES key size in bits (128, 192, or 256)
        """
        self.key_size = key_size // 8  # Convert to bytes
        
    def generate_key(self) -> bytes:
        """Generate a random AES key."""
        return get_random_bytes(self.key_size)
    
    def encrypt_image(self, image: np.ndarray, key: bytes) -> Dict:
        """
        Encrypt an image using AES-256-CBC.
        
        Args:
            image: OpenCV image (numpy array)
            key: AES encryption key
            
        Returns:
            Dictionary containing encrypted data and metadata
        """
        # Convert image to bytes
        _, image_buffer = cv2.imencode('.png', image)
        image_bytes = image_buffer.tobytes()
        
        # Generate random IV (Initialization Vector)
        iv = get_random_bytes(AES.block_size)
        
        # Create cipher and encrypt
        cipher = AES.new(key, AES.MODE_CBC, iv)
        padded_data = pad(image_bytes, AES.block_size)
        encrypted_data = cipher.encrypt(padded_data)
        
        # Store metadata
        return {
            'ciphertext': base64.b64encode(encrypted_data).decode('utf-8'),
            'iv': base64.b64encode(iv).decode('utf-8'),
            'metadata': {
                'original_shape': image.shape,
                'encryption_algorithm': 'AES-256-CBC',
                'key_size': self.key_size * 8,
                'encrypted_size': len(encrypted_data)
            }
        }
    
    def decrypt_image(self, encrypted_data: Dict, key: bytes) -> np.ndarray:
        """
        Decrypt an encrypted image.
        
        Args:
            encrypted_data: Dictionary containing ciphertext and IV
            key: AES decryption key
            
        Returns:
            Decrypted OpenCV image (numpy array)
        """
        # Decode from base64
        ciphertext = base64.b64decode(encrypted_data['ciphertext'])
        iv = base64.b64decode(encrypted_data['iv'])
        
        # Create cipher and decrypt
        cipher = AES.new(key, AES.MODE_CBC, iv)
        decrypted_padded = cipher.decrypt(ciphertext)
        decrypted_data = unpad(decrypted_padded, AES.block_size)
        
        # Convert bytes back to image
        image_array = np.frombuffer(decrypted_data, dtype=np.uint8)
        image = cv2.imdecode(image_array, cv2.IMREAD_COLOR)
        
        return image


class AVSEncryptedStorage:
    """Mock Automated Valet Storage with encrypted image storage."""
    
    def __init__(self):
        self.encryption = ImageEncryption()
        self.storage = {}  # In-memory encrypted storage
        self.keys = {}     # Key storage (in practice, this would be in a KMS)
        
    def store_encrypted_image(self, record_id: str, image: np.ndarray) -> Dict:
        """
        Store an image in encrypted form.
        
        Args:
            record_id: Unique identifier for the image
            image: OpenCV image to encrypt and store
            
        Returns:
            Storage reference with encryption metadata
        """
        # Generate unique key for this image
        key = self.encryption.generate_key()
        
        # Encrypt image
        encrypted_data = self.encryption.encrypt_image(image, key)
        
        # Store encrypted data and key separately
        ciphertext_ref = f"avs://encrypted/blob/{record_id}"
        self.storage[ciphertext_ref] = encrypted_data
        self.keys[record_id] = key
        
        return {
            'record_id': record_id,
            'ciphertext_ref': ciphertext_ref,
            'encryption_metadata': encrypted_data['metadata']
        }
    
    def retrieve_encrypted_image(self, ciphertext_ref: str) -> Optional[Dict]:
        """
        Retrieve encrypted image data (without decrypting).
        
        Args:
            ciphertext_ref: Reference to encrypted blob
            
        Returns:
            Encrypted image data or None if not found
        """
        return self.storage.get(ciphertext_ref)
    
    def get_decryption_key(self, record_id: str, access_context: Dict = None) -> Optional[bytes]:
        """
        Retrieve decryption key (simulates KMS access with authorization).
        
        Args:
            record_id: Record identifier
            access_context: Authorization context from access control
            
        Returns:
            Decryption key or None if not authorized/not found
        """
        # In practice, this would verify access_context against policy
        # before releasing the key from a Key Management Service
        
        if access_context and access_context.get('decision') == 'deny':
            raise PermissionError(f"Access denied for record: {record_id}")
        
        return self.keys.get(record_id)
    
    def decrypt_frame(self, ciphertext_ref: str, record_id: str, access_context: Dict = None) -> Optional[np.ndarray]:
        """
        Retrieve and decrypt a frame (combines retrieval + decryption).
        
        This is the function that would be called by PaCCDetectionService._decrypt_frame()
        
        Args:
            ciphertext_ref: Reference to encrypted blob
            record_id: Record identifier for key lookup
            access_context: Authorization context
            
        Returns:
            Decrypted image or None if not found/not authorized
        """
        # Retrieve encrypted data
        encrypted_data = self.retrieve_encrypted_image(ciphertext_ref)
        if encrypted_data is None:
            return None
        
        # Get decryption key
        key = self.get_decryption_key(record_id, access_context)
        if key is None:
            return None
        
        # Decrypt and return
        return self.encryption.decrypt_image(encrypted_data, key)