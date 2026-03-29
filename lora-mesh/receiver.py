import serial

ser = serial.Serial('COM5', 9600, timeout=1) # Configure gateway node port here

print("Listening for verified data... Press Ctrl+C to stop.")

try:
    with open('received_image_verified.jpg', 'wb') as f:
        while True:
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue

            if line.startswith("METRIC:"):
                print(f"Stats: {line}")

            elif line.startswith("DATA:"):
                parts = line.split(':')
                if len(parts) == 3:
                    binary_data = bytes.fromhex(parts[2])
                    f.write(binary_data)
                    f.flush()
                    print(f"Verified & Stored chunk {parts[1]}")

            elif line.startswith("CORRUPT:"):
                print(f"!!! Warning: Chunk {line.split(':')[1]} was corrupted and discarded.")

            else:
                print(f"[GW]: {line}")  # ← catches NODE messages and anything unexpected

except KeyboardInterrupt:
    print("\nTransfer ended.")