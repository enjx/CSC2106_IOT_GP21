import serial
import time

PORT = 'COM9' # Insert port no. used by your sender node
BAUD = 9600    
FILENAME = 'image.jpg'
CHUNK_SIZE = 42

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(2) # Wait for Arduino to reset

with open(FILENAME, 'rb') as f:
    chunk_idx = 0
    
    while True:
        data = f.read(CHUNK_SIZE)
        if not data:
            break

        # Ensure data is exactly 42 bytes for the Arduino buffer
        if len(data) < CHUNK_SIZE:
            data = data + b'\x00' * (CHUNK_SIZE - len(data))
        
        ser.write(data)
        
        while True:
            line = ser.readline().decode(errors='ignore').strip()
            if line:
                print(f"[ARDUINO]: {line}")  # Print everything
            if f"ACK:{chunk_idx}" in line:
                print(f"Sent chunk {chunk_idx}")
                chunk_idx += 1
                break
            elif "RETRY" in line:
                print(f"Retrying chunk {chunk_idx}...")
                ser.write(data)