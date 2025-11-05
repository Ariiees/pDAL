#!/bin/bash

# Test script for AES-GCM Frame Encryption
# This creates sample images, encrypts them, and decrypts them to verify correctness

set -e  # Exit on error

echo "=== AES-GCM Frame Encryption Test ==="
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if executables exist
if [ ! -f "./encrypt_frames" ] || [ ! -f "./decrypt_frames" ]; then
    echo -e "${RED}Error: Executables not found. Please build the project first.${NC}"
    echo "Run: mkdir build && cd build && cmake .. && cmake --build ."
    exit 1
fi

# Create test directories
TEST_DIR="./test_frames"
ENCRYPTED_DIR="./test_encrypted"
DECRYPTED_DIR="./test_decrypted"

echo "Creating test directories..."
mkdir -p "$TEST_DIR"
rm -rf "$ENCRYPTED_DIR" "$DECRYPTED_DIR"

# Create sample test images using ImageMagick or Python
echo "Generating sample test images..."

if command -v convert &> /dev/null; then
    # Using ImageMagick
    for i in {1..5}; do
        convert -size 1024x768 xc:blue \
                -fill white -pointsize 100 -gravity center \
                -annotate +0+0 "Frame $i" \
                "$TEST_DIR/test_frame_$(printf "%03d" $i).png"
    done
    echo "Created 5 test images using ImageMagick"
elif command -v python3 &> /dev/null; then
    # Using Python with PIL
    python3 - <<EOF
from PIL import Image, ImageDraw, ImageFont
import os

test_dir = "$TEST_DIR"
for i in range(1, 6):
    img = Image.new('RGB', (1024, 768), color='blue')
    draw = ImageDraw.Draw(img)
    
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 80)
    except:
        font = ImageFont.load_default()
    
    text = f"Frame {i}"
    # Get text bounding box
    bbox = draw.textbbox((0, 0), text, font=font)
    text_width = bbox[2] - bbox[0]
    text_height = bbox[3] - bbox[1]
    
    position = ((1024 - text_width) // 2, (768 - text_height) // 2)
    draw.text(position, text, fill='white', font=font)
    
    filename = os.path.join(test_dir, f"test_frame_{i:03d}.png")
    img.save(filename)
    print(f"Created {filename}")
EOF
    echo "Created 5 test images using Python PIL"
else
    echo -e "${YELLOW}Warning: Neither ImageMagick nor Python PIL found.${NC}"
    echo "Creating dummy files instead..."
    for i in {1..5}; do
        dd if=/dev/urandom of="$TEST_DIR/test_frame_$(printf "%03d" $i).png" bs=1M count=1 2>/dev/null
    done
    echo "Created 5 dummy files (1MB each)"
fi

echo ""
echo "=== Step 1: Encryption ==="
./encrypt_frames "$TEST_DIR" "$ENCRYPTED_DIR"

echo ""
echo "=== Step 2: Decryption ==="
./decrypt_frames "$ENCRYPTED_DIR" "$ENCRYPTED_DIR/encryption_key.bin" "$DECRYPTED_DIR"

echo ""
echo "=== Step 3: Verification ==="
VERIFICATION_PASSED=true

for file in "$TEST_DIR"/*.png; do
    filename=$(basename "$file")
    original="$TEST_DIR/$filename"
    decrypted="$DECRYPTED_DIR/$filename"
    
    if [ -f "$decrypted" ]; then
        if cmp -s "$original" "$decrypted"; then
            echo -e "${GREEN}✓${NC} $filename: Match"
        else
            echo -e "${RED}✗${NC} $filename: Mismatch!"
            VERIFICATION_PASSED=false
        fi
    else
        echo -e "${RED}✗${NC} $filename: Decrypted file not found!"
        VERIFICATION_PASSED=false
    fi
done

echo ""
if [ "$VERIFICATION_PASSED" = true ]; then
    echo -e "${GREEN}=== ALL TESTS PASSED ===${NC}"
    echo "Encryption and decryption working correctly!"
else
    echo -e "${RED}=== TESTS FAILED ===${NC}"
    echo "Some files did not match after decryption."
    exit 1
fi

echo ""
echo "File sizes:"
echo "Original: $(du -sh $TEST_DIR | cut -f1)"
echo "Encrypted: $(du -sh $ENCRYPTED_DIR | cut -f1)"
echo "Decrypted: $(du -sh $DECRYPTED_DIR | cut -f1)"

echo ""
echo -e "${YELLOW}Tip:${NC} You can now test with your own KITTI frames:"
echo "  ./encrypt_frames /path/to/kitti_frames ./encrypted_output"
