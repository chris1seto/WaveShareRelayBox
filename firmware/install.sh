#!/bin/bash

# Waveshare ESP32-S3 Relay Firmware Installation Script

set -e

echo "Waveshare ESP32-S3 Relay Firmware Installation"
echo "=============================================="
echo ""

# Check if we're in the right directory
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: Please run this script from the project root directory"
    exit 1
fi

# Check if ESP-IDF is installed
if ! command -v idf.py &> /dev/null; then
    echo "ESP-IDF not found. Installing ESP-IDF..."
    echo ""
    echo "Please follow the ESP-IDF installation guide:"
    echo "https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/"
    echo ""
    echo "After installation, source the environment:"
    echo "source \$HOME/.espressif/esp-idf/export.sh"
    echo ""
    exit 1
fi

echo "ESP-IDF found. Checking version..."
idf.py --version

echo ""
echo "Installing Python dependencies for examples..."
if [ -d "examples" ]; then
    cd examples
    if command -v pip3 &> /dev/null; then
        pip3 install -r requirements.txt
    elif command -v pip &> /dev/null; then
        pip install -r requirements.txt
    else
        echo "Warning: pip not found. Please install Python dependencies manually:"
        echo "pip install -r examples/requirements.txt"
    fi
    cd ..
fi

echo ""
echo "Configuration:"
echo "1. Edit sdkconfig.defaults to set your WiFi credentials"
echo "2. Verify GPIO pin assignments in main/relay_control.h"
echo "3. Configure MQTT broker settings in main/mqtt_client.h"
echo ""
echo "Build commands:"
echo "  ./build.sh          # Build the project"
echo "  idf.py flash        # Flash to device"
echo "  idf.py monitor      # Monitor serial output"
echo ""
echo "Installation complete!"


