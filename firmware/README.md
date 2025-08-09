# Waveshare ESP32-S3 Relay Firmware

A custom firmware for the Waveshare ESP32-S3-Relay-6CH board that provides WiFi connectivity, web-based control interface, MQTT integration, and OTA update capabilities.

## Features

- **WiFi Connectivity**: Connects to existing WiFi networks or creates an access point for configuration
- **Web UI**: Modern, responsive web interface for relay control and system monitoring
- **MQTT Client**: Publishes relay status and accepts control commands via MQTT
- **Relay Control**: Individual and batch control of 6 relays via web interface or MQTT
- **OTA Updates**: Over-the-air firmware updates via web interface
- **System Monitoring**: Real-time status monitoring including WiFi, MQTT, memory, and uptime

## Hardware Requirements

- Waveshare ESP32-S3-Relay-6CH board
- USB-C cable for programming and power
- WiFi network (optional, device can run in AP mode)

## Software Requirements

- ESP-IDF v5.0 or later
- Python 3.7 or later
- Git

## Installation and Setup

### 1. Clone the Repository

```bash
git clone <repository-url>
cd waveshare-relay-firmware
```

### 2. Install ESP-IDF

Follow the official ESP-IDF installation guide: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/

### 3. Set up the Environment

```bash
# Source the ESP-IDF environment
source $HOME/.espressif/esp-idf/export.sh

# Or if using the ESP-IDF installer
source ~/esp/esp-idf/export.sh
```

### 4. Configure the Project

```bash
# Configure the project
idf.py menuconfig
```

Key configuration options:
- **WiFi Settings**: Set your WiFi SSID and password
- **MQTT Settings**: Configure MQTT broker URL and credentials
- **GPIO Pins**: Verify relay GPIO pin assignments match your board

### 5. Build and Flash

```bash
# Build the project
idf.py build

# Flash to the device
idf.py flash

# Monitor serial output
idf.py monitor
```

## Usage

### Initial Setup

1. **First Boot**: The device will start in AP mode with SSID "Waveshare-Relay-AP" and password "12345678"
2. **Connect to AP**: Connect your computer/phone to the AP
3. **Access Web Interface**: Open a browser and navigate to `http://192.168.4.1`
4. **Configure WiFi**: Use the WiFi configuration tab to set up your network credentials
5. **Save Settings**: The device will restart and connect to your WiFi network

### Web Interface

The web interface provides four main sections:

#### 1. Relay Control
- Individual control of each relay (ON/OFF)
- Visual status indicators
- Real-time state updates

#### 2. System Status
- WiFi connection status
- IP address
- MQTT connection status
- Free memory
- Uptime
- Firmware version

#### 3. WiFi Configuration
- Set WiFi SSID and password
- Save credentials to NVS
- Automatic reconnection

#### 4. OTA Update
- Upload new firmware files (.bin)
- Automatic restart after successful update

### MQTT Integration

The device publishes and subscribes to MQTT topics for remote control:

#### Topics
- **Status**: `waveshare/relay/status` - Current relay states
- **Control**: `waveshare/relay/set` - Relay control commands
- **Config**: `waveshare/relay/config` - Device configuration

#### Control Messages

Send JSON messages to `waveshare/relay/set` to control relays:

```json
// Turn on relay 0 and turn off relay 1
{"0": true, "1": false}

// Turn on only relay 2
{"2": true}

// Turn off all relays
{"0": false, "1": false, "2": false, "3": false, "4": false, "5": false}
```

#### Status Messages

The device publishes status updates to `waveshare/relay/status`:

```json
{
  "0": true,
  "1": false,
  "2": true,
  "3": false,
  "4": false,
  "5": true
}
```

## Configuration

### WiFi Settings

The device supports two modes:
- **Station Mode**: Connects to existing WiFi network
- **Access Point Mode**: Creates its own network for configuration

WiFi credentials are stored in NVS and persist across reboots.

### MQTT Settings

Configure MQTT broker settings in `main/mqtt_client.h`:

```c
#define MQTT_BROKER_URL "mqtt://192.168.1.100"
#define MQTT_BROKER_PORT 1883
#define MQTT_CLIENT_ID "waveshare-relay-esp32s3"
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""
```

### GPIO Configuration

Relay GPIO pins are configured in `main/relay_control.h`:

```c
#define RELAY_1_GPIO GPIO_NUM_2
#define RELAY_2_GPIO GPIO_NUM_3
#define RELAY_3_GPIO GPIO_NUM_4
#define RELAY_4_GPIO GPIO_NUM_5
#define RELAY_5_GPIO GPIO_NUM_6
#define RELAY_6_GPIO GPIO_NUM_7
```

**Important**: Verify these GPIO assignments match your specific board layout.

## Development

### Project Structure

```
waveshare-relay-firmware/
├── main/                    # Main application code
│   ├── main.c              # Application entry point
│   ├── relay_control.c     # Relay GPIO control
│   ├── wifi_manager.c      # WiFi connection management
│   ├── mqtt_client.c       # MQTT client implementation
│   ├── web_server.c        # HTTP server and web UI
│   └── ota_update.c        # OTA update functionality
├── CMakeLists.txt          # Main CMake configuration
├── sdkconfig.defaults      # Default SDK configuration
├── partitions.csv          # Partition table
└── README.md              # This file
```

### Building

```bash
# Clean build
idf.py clean

# Build with verbose output
idf.py build -v

# Build and flash
idf.py build flash

# Build, flash, and monitor
idf.py build flash monitor
```

### Debugging

```bash
# Monitor serial output
idf.py monitor

# Monitor with specific baud rate
idf.py monitor -b 115200

# Monitor with filter
idf.py monitor --monitor-filter "RELAY_CONTROL"
```

## Troubleshooting

### Common Issues

1. **WiFi Connection Fails**
   - Check SSID and password in configuration
   - Verify network is 2.4GHz (ESP32-S3 doesn't support 5GHz)
   - Check signal strength

2. **MQTT Connection Issues**
   - Verify broker URL and port
   - Check network connectivity
   - Ensure broker is running and accessible

3. **Relay Not Responding**
   - Verify GPIO pin assignments
   - Check hardware connections
   - Monitor serial output for errors

4. **OTA Update Fails**
   - Ensure firmware file is valid
   - Check partition table configuration
   - Verify sufficient flash space

### Log Levels

Adjust log levels in `sdkconfig.defaults`:

```bash
# Set log level
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y
```

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

## Support

For issues and questions:
- Check the troubleshooting section
- Review the ESP-IDF documentation
- Open an issue on the project repository

## Changelog

### v1.0.0
- Initial release
- WiFi connectivity (STA/AP modes)
- Web-based relay control interface
- MQTT integration
- OTA update capability
- System status monitoring
