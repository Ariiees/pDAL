#!/bin/bash
# Automated setup script for Ubuntu
# This script installs dependencies, builds the project, and runs tests

set -e  # Exit on error

echo "========================================"
echo "  AES-GCM Frame Encryption - Ubuntu Setup"
echo "========================================"
echo ""

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if running on Ubuntu/Debian
if [ ! -f /etc/debian_version ]; then
    echo -e "${YELLOW}Warning: This script is designed for Ubuntu/Debian${NC}"
    echo "It may work on other distributions but is not tested."
    read -p "Continue anyway? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Check if running as root
if [ "$EUID" -eq 0 ]; then 
    echo -e "${RED}Error: Do not run this script as root${NC}"
    echo "The script will ask for sudo password when needed."
    exit 1
fi

echo -e "${BLUE}Step 1: Installing dependencies...${NC}"
echo ""

# Check if packages are already installed
PACKAGES_NEEDED=""
for pkg in build-essential cmake libssl-dev; do
    if ! dpkg -l | grep -q "^ii  $pkg "; then
        PACKAGES_NEEDED="$PACKAGES_NEEDED $pkg"
    fi
done

if [ -n "$PACKAGES_NEEDED" ]; then
    echo "The following packages will be installed:$PACKAGES_NEEDED"
    echo "This requires sudo privileges."
    echo ""
    
    sudo apt-get update
    sudo apt-get install -y $PACKAGES_NEEDED
    
    echo -e "${GREEN}✓ Dependencies installed${NC}"
else
    echo -e "${GREEN}✓ All dependencies already installed${NC}"
fi

echo ""
echo -e "${BLUE}Step 2: Checking for AES-NI support...${NC}"
if grep -q aes /proc/cpuinfo; then
    echo -e "${GREEN}✓ AES-NI hardware acceleration: SUPPORTED${NC}"
    echo "  Expected performance: 1-2 GB/s"
else
    echo -e "${YELLOW}⚠ AES-NI hardware acceleration: NOT DETECTED${NC}"
    echo "  Encryption will still work but may be slower (5-10x)"
fi

echo ""
echo -e "${BLUE}Step 3: Building the project...${NC}"
echo ""

# Create build directory
if [ ! -d "build" ]; then
    mkdir build
else
    echo "Build directory exists, cleaning..."
    rm -rf build/*
fi

cd build

# Configure with CMake
echo "Configuring with CMake..."
cmake -DCMAKE_BUILD_TYPE=Release .. || {
    echo -e "${RED}✗ CMake configuration failed${NC}"
    exit 1
}

# Build
echo "Building (using all CPU cores)..."
make -j$(nproc) || {
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
}

echo -e "${GREEN}✓ Build successful${NC}"

# Check if executables were created
if [ ! -f "encrypt_frames" ] || [ ! -f "decrypt_frames" ]; then
    echo -e "${RED}✗ Executables not found${NC}"
    exit 1
fi

echo ""
echo -e "${BLUE}Step 4: Running tests...${NC}"
echo ""

cd ..

# Make test script executable
chmod +x test_encryption.sh

# Run tests
if ./test_encryption.sh; then
    echo ""
    echo -e "${GREEN}========================================"
    echo "  ✓ SETUP COMPLETE!"
    echo "========================================${NC}"
    echo ""
    echo "Your encryption tools are ready to use!"
    echo ""
    echo -e "${BLUE}Quick Start:${NC}"
    echo "  cd build"
    echo "  ./encrypt_frames /path/to/kitti_frames ./encrypted_output"
    echo ""
    echo -e "${BLUE}Example with KITTI:${NC}"
    echo "  ./encrypt_frames ~/datasets/kitti/2011_09_26/drive_0001/image_00/data ./encrypted_kitti"
    echo ""
    echo -e "${BLUE}Executables location:${NC}"
    echo "  $(pwd)/build/encrypt_frames"
    echo "  $(pwd)/build/decrypt_frames"
    echo ""
    echo -e "${BLUE}Documentation:${NC}"
    echo "  • Quick start: QUICKSTART.md"
    echo "  • Ubuntu guide: UBUNTU_GUIDE.md"
    echo "  • Full docs: README.md"
    echo ""
    
    # Check AES-NI again and give performance estimate
    if grep -q aes /proc/cpuinfo; then
        echo -e "${GREEN}Performance:${NC}"
        echo "  • ~1 ms per 1.2MB frame"
        echo "  • 1000+ frames/second"
        echo "  • Real-time capable ✓"
    fi
    
    echo ""
else
    echo ""
    echo -e "${RED}========================================"
    echo "  ✗ Tests failed"
    echo "========================================${NC}"
    echo ""
    echo "The build completed but tests failed."
    echo "This might indicate a problem with the installation."
    echo ""
    echo "Try running manually:"
    echo "  cd build"
    echo "  ./encrypt_frames"
    echo ""
    exit 1
fi
