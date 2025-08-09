#!/bin/bash

# Waveshare ESP32-S3 Relay Firmware Build Script

set -e

echo "Building Waveshare ESP32-S3 Relay Firmware..."

# Check if ESP-IDF is sourced
if ! command -v idf.py &> /dev/null; then
    echo "Error: ESP-IDF not found. Please source the ESP-IDF environment first:"
    echo "source \$HOME/.espressif/esp-idf/export.sh"
    exit 1
fi

# Clean previous build
echo "Cleaning previous build..."
idf.py clean

# Build the project
echo "Building project..."
idf.py build

echo "Build completed successfully!"
echo ""
echo "To flash the firmware:"
echo "  idf.py flash"
echo ""
echo "To monitor serial output:"
echo "  idf.py monitor"
echo ""
echo "To build, flash, and monitor:"
echo "  idf.py build flash monitor"


