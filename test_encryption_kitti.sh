#!/bin/bash

# Test script for AES-GCM Frame Encryption using existing images
# This tests encryption and decryption with your own image folder

set -e  # Exit on error

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== AES-GCM Frame Encryption Test (Using Your Images) ===${NC}"
echo ""

# Check if folder argument provided
if [ -z "$1" ]; then
    echo -e "${YELLOW}Usage: $0 <path_to_image_folder>${NC}"
    echo ""
    echo "Example:"
    echo "  $0 /path/to/your/images"
    echo "  $0 ~/kitti_frames/sample"
    echo ""
    exit 1
fi

INPUT_FOLDER="$1"

# Check if folder exists
if [ ! -d "$INPUT_FOLDER" ]; then
    echo -e "${RED}Error: Folder does not exist: $INPUT_FOLDER${NC}"
    exit 1
fi

# Check if folder has images
IMAGE_COUNT=$(find "$INPUT_FOLDER" -maxdepth 1 -type f \( -iname "*.png" -o -iname "*.jpg" -o -iname "*.jpeg" -o -iname "*.bmp" -o -iname "*.tiff" \) | wc -l)

if [ "$IMAGE_COUNT" -eq 0 ]; then
    echo -e "${RED}Error: No image files found in: $INPUT_FOLDER${NC}"
    echo "Looking for: .png, .jpg, .jpeg, .bmp, .tiff files"
    exit 1
fi

echo -e "${GREEN}✓ Found $IMAGE_COUNT image(s) in: $INPUT_FOLDER${NC}"
echo ""

# Check if executables exist
if [ ! -f "./build/encrypt_frames" ] || [ ! -f "./build/decrypt_frames" ]; then
    echo -e "${RED}Error: Executables not found.${NC}"
    echo "Please build the project first:"
    echo "  mkdir build && cd build && cmake .. && make"
    exit 1
fi

# Create test directories
TEST_ENCRYPTED="./test_encrypted_output"
TEST_DECRYPTED="./test_decrypted_output"

echo -e "${BLUE}Creating test directories...${NC}"
rm -rf "$TEST_ENCRYPTED" "$TEST_DECRYPTED"
mkdir -p "$TEST_ENCRYPTED" "$TEST_DECRYPTED"

echo ""
echo -e "${BLUE}=== Step 1: Encrypting Images ===${NC}"
echo ""

./build/encrypt_frames "$INPUT_FOLDER" "$TEST_ENCRYPTED"
ENCRYPT_STATUS=$?

if [ $ENCRYPT_STATUS -ne 0 ]; then
    echo ""
    echo -e "${RED}✗ Encryption failed!${NC}"
    exit 1
fi

echo ""
echo -e "${BLUE}=== Step 2: Decrypting Images ===${NC}"
echo ""

./build/decrypt_frames "$TEST_ENCRYPTED" "$TEST_ENCRYPTED/encryption_key.bin" "$TEST_DECRYPTED"
DECRYPT_STATUS=$?

if [ $DECRYPT_STATUS -ne 0 ]; then
    echo ""
    echo -e "${RED}✗ Decryption failed!${NC}"
    exit 1
fi

echo ""
echo -e "${BLUE}=== Step 3: Verification ===${NC}"
echo ""

VERIFICATION_PASSED=true
MATCH_COUNT=0
MISMATCH_COUNT=0

# Compare each file
for original_file in "$INPUT_FOLDER"/*; do
    if [ -f "$original_file" ]; then
        filename=$(basename "$original_file")
        decrypted_file="$TEST_DECRYPTED/$filename"
        
        if [ -f "$decrypted_file" ]; then
            if cmp -s "$original_file" "$decrypted_file"; then
                echo -e "${GREEN}✓${NC} $filename: Match"
                MATCH_COUNT=$((MATCH_COUNT + 1))
            else
                echo -e "${RED}✗${NC} $filename: Mismatch!"
                VERIFICATION_PASSED=false
                MISMATCH_COUNT=$((MISMATCH_COUNT + 1))
            fi
        else
            # Check if it's an image file that should have been encrypted
            ext="${filename##*.}"
            ext_lower=$(echo "$ext" | tr '[:upper:]' '[:lower:]')
            if [[ "$ext_lower" == "png" || "$ext_lower" == "jpg" || "$ext_lower" == "jpeg" || "$ext_lower" == "bmp" || "$ext_lower" == "tiff" ]]; then
                echo -e "${RED}✗${NC} $filename: Decrypted file not found!"
                VERIFICATION_PASSED=false
                MISMATCH_COUNT=$((MISMATCH_COUNT + 1))
            fi
        fi
    fi
done

echo ""
if [ "$VERIFICATION_PASSED" = true ] && [ $MATCH_COUNT -gt 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  ✓ ALL TESTS PASSED${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "Encryption and decryption working correctly!"
    echo ""
    echo "Summary:"
    echo "  Files tested: $MATCH_COUNT"
    echo "  All files matched: YES"
    echo ""
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}  ✗ TESTS FAILED${NC}"
    echo -e "${RED}========================================${NC}"
    echo ""
    echo "Some files did not match after decryption."
    echo ""
    echo "Summary:"
    echo "  Files matched: $MATCH_COUNT"
    echo "  Files mismatched: $MISMATCH_COUNT"
    echo ""
    exit 1
fi

# Show file sizes
echo "File sizes:"
ORIGINAL_SIZE=$(du -sh "$INPUT_FOLDER" | cut -f1)
ENCRYPTED_SIZE=$(du -sh "$TEST_ENCRYPTED" | cut -f1)
DECRYPTED_SIZE=$(du -sh "$TEST_DECRYPTED" | cut -f1)

echo "  Original:  $ORIGINAL_SIZE"
echo "  Encrypted: $ENCRYPTED_SIZE"
echo "  Decrypted: $DECRYPTED_SIZE"

echo ""
echo -e "${BLUE}Test artifacts created:${NC}"
echo "  Encrypted data: $TEST_ENCRYPTED"
echo "  Decrypted data: $TEST_DECRYPTED"
echo ""
echo -e "${YELLOW}To clean up test files, run:${NC}"
echo "  rm -rf $TEST_ENCRYPTED $TEST_DECRYPTED"
echo ""
echo -e "${GREEN}✓ Your encryption system is working correctly!${NC}"
echo ""
echo -e "${BLUE}You can now encrypt your KITTI frames:${NC}"
echo "  ./build/encrypt_frames /path/to/kitti_frames ./encrypted_output"
