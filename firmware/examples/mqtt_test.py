#!/usr/bin/env python3
"""
MQTT Test Script for Waveshare ESP32-S3 Relay Control

This script demonstrates how to control the relays via MQTT.
"""

import paho.mqtt.client as mqtt
import json
import time
import sys

# MQTT Configuration
MQTT_BROKER = "192.168.1.100"  # Change to your MQTT broker IP
MQTT_PORT = 1883
MQTT_TOPIC_SET = "waveshare/relay/set"
MQTT_TOPIC_STATUS = "waveshare/relay/status"

def on_connect(client, userdata, flags, rc):
    """Callback when connected to MQTT broker"""
    print(f"Connected to MQTT broker with result code {rc}")
    # Subscribe to status topic
    client.subscribe(MQTT_TOPIC_STATUS)
    print(f"Subscribed to {MQTT_TOPIC_STATUS}")

def on_message(client, userdata, msg):
    """Callback when message received"""
    try:
        data = json.loads(msg.payload.decode())
        print(f"Relay Status: {data}")
    except json.JSONDecodeError:
        print(f"Invalid JSON received: {msg.payload}")

def on_disconnect(client, userdata, rc):
    """Callback when disconnected from MQTT broker"""
    print(f"Disconnected from MQTT broker with result code {rc}")

def control_relay(client, relay_id, state):
    """Control a single relay"""
    message = {str(relay_id): state}
    client.publish(MQTT_TOPIC_SET, json.dumps(message))
    print(f"Sent command: {message}")

def control_multiple_relays(client, relay_states):
    """Control multiple relays at once"""
    message = {str(k): v for k, v in relay_states.items()}
    client.publish(MQTT_TOPIC_SET, json.dumps(message))
    print(f"Sent command: {message}")

def main():
    # Create MQTT client
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    # Connect to MQTT broker
    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
    except Exception as e:
        print(f"Failed to connect to MQTT broker: {e}")
        print("Please check your MQTT broker configuration")
        sys.exit(1)

    # Start the network loop
    client.loop_start()

    print("MQTT Relay Control Test")
    print("=======================")
    print("Commands:")
    print("  1-6: Toggle relay 1-6")
    print("  a: Turn on all relays")
    print("  o: Turn off all relays")
    print("  s: Show current status")
    print("  q: Quit")
    print()

    try:
        while True:
            command = input("Enter command: ").strip().lower()
            
            if command == 'q':
                break
            elif command == 's':
                print("Requesting current status...")
                # Status is automatically published by the device
            elif command == 'a':
                control_multiple_relays(client, {i: True for i in range(6)})
            elif command == 'o':
                control_multiple_relays(client, {i: False for i in range(6)})
            elif command in ['1', '2', '3', '4', '5', '6']:
                relay_id = int(command) - 1  # Convert to 0-based index
                print(f"Toggling relay {relay_id + 1}")
                # For demo purposes, we'll just turn it on
                # In a real application, you might want to track the current state
                control_relay(client, relay_id, True)
            else:
                print("Invalid command. Use 1-6, a, o, s, or q.")

    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        client.loop_stop()
        client.disconnect()

if __name__ == "__main__":
    main()


