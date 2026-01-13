#!/usr/bin/env bash
set -euo pipefail

# setup_ubuntu_for_avs_pacc_bringup.sh
#
# Installs Ubuntu and ROS 2 dependencies for the following ament CMake packages:
#   - avs: rclcpp, sensor_msgs, cv_bridge, OpenCV, yaml-cpp, sqlite3 (pkg-config)
#   - pacc: OpenSSL (libcrypto)
#   - bring_up: depends on avs + pacc, plus rclcpp/sensor_msgs/cv_bridge
#
# Usage:
#   bash setup_ubuntu_for_avs_pacc_bringup.sh [ros_distro] [ws_dir]
#
# Examples:
#   bash setup_ubuntu_for_avs_pacc_bringup.sh humble ~/PaCC
#   bash setup_ubuntu_for_avs_pacc_bringup.sh jazzy  ~/PaCC

ROS_DISTRO="${1:-humble}"
WS_DIR="${2:-$HOME/PaCC}"

need_cmd() { command -v "$1" >/dev/null 2>&1; }

sudo apt-get update

# Core build tools
sudo apt-get install -y \
  build-essential cmake git pkg-config \
  python3 python3-pip python3-venv \
  python3-colcon-common-extensions \
  python3-rosdep \
  clang-format

# ROS 2 base (assumes ROS 2 apt repo already configured; if not, install ros-$ROS_DISTRO-desktop below will fail)
sudo apt-get install -y \
  "ros-${ROS_DISTRO}-desktop" \
  "ros-${ROS_DISTRO}-ament-cmake" \
  "ros-${ROS_DISTRO}-rclcpp" \
  "ros-${ROS_DISTRO}-sensor-msgs" \
  "ros-${ROS_DISTRO}-cv-bridge"

# Non-ROS system libs required by your CMakeLists
sudo apt-get install -y \
  libopencv-dev \
  libyaml-cpp-dev \
  libsqlite3-dev \
  libssl-dev

# Initialize rosdep (idempotent)
if ! sudo test -f /etc/ros/rosdep/sources.list.d/20-default.list; then
  sudo rosdep init
fi
rosdep update

# Source ROS environment for this shell and persist it for future shells
ROS_SETUP="/opt/ros/${ROS_DISTRO}/setup.bash"
if [[ ! -f "${ROS_SETUP}" ]]; then
  echo "ERROR: ${ROS_SETUP} not found. Check ROS distro name or ROS installation."
  exit 1
fi
# shellcheck disable=SC1090
source "${ROS_SETUP}"

if ! grep -qs "source ${ROS_SETUP}" "$HOME/.bashrc"; then
  echo "source ${ROS_SETUP}" >> "$HOME/.bashrc"
fi

# Workspace prep
mkdir -p "${WS_DIR}/src"
cd "${WS_DIR}"

# Install any additional deps declared in package.xml (recommended even if apt installs above already cover them)
# This expects your ROS 2 packages are under ${WS_DIR}/src
rosdep install -y --from-paths src --ignore-src --rosdistro "${ROS_DISTRO}"

# Build
colcon build --symlink-install

# Source workspace overlay for this shell and persist it
OVERLAY_SETUP="${WS_DIR}/install/setup.bash"
# shellcheck disable=SC1090
source "${OVERLAY_SETUP}"

if ! grep -qs "source ${OVERLAY_SETUP}" "$HOME/.bashrc"; then
  echo "source ${OVERLAY_SETUP}" >> "$HOME/.bashrc"
fi

echo
echo "Done."
echo "ROS_DISTRO=${ROS_DISTRO}"
echo "Workspace=${WS_DIR}"
echo "In a new terminal, ROS and the workspace will be sourced automatically."
echo
echo "Quick checks:"
echo "  pkg-config --modversion sqlite3"
echo "  python3 -c 'import cv2; print(cv2.__version__)'"
echo "  ros2 pkg list | grep -E 'avs|pacc|bring_up' || true"
