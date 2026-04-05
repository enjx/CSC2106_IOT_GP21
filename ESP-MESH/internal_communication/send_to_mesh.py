"""
send_to_mesh.py
---------------
Sends a message from your PC through a connected ESP32 node into the ESP-MESH.

Usage:
    python send_to_mesh.py COM4 "Hello from PC!"
    python send_to_mesh.py COM4          (interactive mode)

Install dependency first:
    pip install pyserial
"""

import sys
import serial
import time

def open_port(port, baud=115200):
    ser = serial.Serial()
    ser.port     = port
    ser.baudrate = baud
    ser.timeout  = 2
    ser.open()
    # Disable DTR/RTS AFTER opening — prevents ESP32 auto-reset
    ser.dtr = False
    ser.rts = False
    print("Waiting 10s for ESP32 to reboot and rejoin mesh...")
    time.sleep(10)
    return ser

def send_message(port, message):
    try:
        with open_port(port) as ser:
            payload = message.strip() + '\n'
            ser.write(payload.encode('utf-8'))
            ser.flush()
            print(f"[PC→NODE] Sent: {message}")

            # Read responses for 2 seconds
            deadline = time.time() + 2.0
            while time.time() < deadline:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if line:
                    print(f"[NODE]    {line}")
    except serial.SerialException as e:
        print(f"Error: {e}")
        print("Make sure idf.py monitor is NOT running (it blocks the port)")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python send_to_mesh.py <COM_PORT> [message]")
        print("Example: python send_to_mesh.py COM4 'Hello mesh!'")
        sys.exit(1)

    port = sys.argv[1]

    if len(sys.argv) >= 3:
        msg = ' '.join(sys.argv[2:])
        send_message(port, msg)
    else:
        # Interactive mode
        print(f"Connected to {port}. Type messages and press Enter. Ctrl+C to quit.\n")
        try:
            with open_port(port) as ser:
                while True:
                    msg = input("Message: ").strip()
                    if msg:
                        ser.write((msg + '\n').encode('utf-8'))
                        ser.flush()
                        print(f"[PC→NODE] Sent: {msg}")
                        time.sleep(0.5)
                        while ser.in_waiting:
                            line = ser.readline().decode('utf-8', errors='replace').strip()
                            if line:
                                print(f"[NODE]    {line}")
        except KeyboardInterrupt:
            print("\nDone.")
        except serial.SerialException as e:
            print(f"Error: {e}")
