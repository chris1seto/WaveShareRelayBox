#!/usr/bin/env python3
"""Interactive MQTT test client for the Waveshare relay box.

    pip install -r requirements.txt
    python mqtt_test.py --broker 192.168.1.10 [--root waveshare/relay]

Commands: 0-5 toggles a relay, "a" all on, "o" all off, "q" quits.
"""
import argparse
import json

import paho.mqtt.client as mqtt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--root", default="waveshare/relay", help="root topic configured on the device")
    ap.add_argument("--username")
    ap.add_argument("--password")
    args = ap.parse_args()
    root = args.root.rstrip("/")

    def on_connect(client, userdata, flags, reason_code, properties):
        print(f"Connected ({reason_code}); watching {root}/#")
        client.subscribe(f"{root}/status")
        client.subscribe(f"{root}/availability")

    def on_message(client, userdata, msg):
        print(f"{msg.topic}: {msg.payload.decode(errors='replace')}")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if args.username:
        client.username_pw_set(args.username, args.password)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.broker, args.port, 60)
    client.loop_start()

    try:
        while True:
            cmd = input("> ").strip().lower()
            if cmd == "q":
                break
            if cmd in ("a", "o"):
                client.publish(f"{root}/set", json.dumps({str(i): cmd == "a" for i in range(6)}))
            elif cmd in [str(i) for i in range(6)]:
                client.publish(f"{root}/relay/{cmd}/set", "TOGGLE")
            else:
                print("0-5 = toggle relay, a = all on, o = all off, q = quit")
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
