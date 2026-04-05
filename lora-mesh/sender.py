import serial
import time

PORT = 'COM9' # Insert port no. used by your sender node
BAUD = 9600    
FILENAME = 'image.jpg'
CHUNK_SIZE = 50  # Match LORA_PAYLOAD in mesh_node.ino (must fit in 64-byte serial buffer)

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(2) # Wait for Arduino to reset

with open(FILENAME, 'rb') as f:
    chunk_idx = 0
    
    while True:
        data = f.read(CHUNK_SIZE)
        if not data:
            break

        # Ensure data matches size for the Arduino buffer
        if len(data) < CHUNK_SIZE:
            data = data + b'\x00' * (CHUNK_SIZE - len(data))
        
        ser.write(data)
        
        # Wait for ACK or FAIL from Arduino (retries handled by mesh node)
        while True:
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue
            print(f"[NODE1]: {line}")
            if f"ACK:{chunk_idx}" in line:
                chunk_idx += 1
                break
            elif line.startswith("FAIL:"):
                print(f"ERROR: Chunk {chunk_idx} failed after all retries. Aborting.")
                exit(1)