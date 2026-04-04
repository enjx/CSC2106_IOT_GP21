import serial
import time

PORT = 'COM9' # Insert port no. used by your sender node
BAUD = 9600    
FILENAME = 'image.jpg'
CHUNK_SIZE = 42  # Match LORA_PAYLOAD in mesh_node.ino

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(2) # Wait for Arduino to reset

with open(FILENAME, 'rb') as f:
    chunk_idx = 0
    
    while True:
        data = f.read(CHUNK_SIZE)
        if not data:
            break

        # Ensure data is matches size for the Arduino buffer
        if len(data) < CHUNK_SIZE:
            data = data + b'\x00' * (CHUNK_SIZE - len(data))
        
        ser.write(data)
        retry_count = 0
        MAX_RETRIES = 3  # Match RadioHead library default
        
        while True:
            line = ser.readline().decode(errors='ignore').strip()
            if not line:
                continue
            print(f"[NODE1]: {line}")  # print everything
            if f"ACK:{chunk_idx}" in line:
                chunk_idx += 1
                break
            elif "RETRY" in line:
                retry_count += 1
                if retry_count <= MAX_RETRIES:
                    print(f"Retrying chunk {chunk_idx}... (attempt {retry_count}/{MAX_RETRIES})")
                    ser.write(data)
                else:
                    print(f"ERROR: Chunk {chunk_idx} failed after {MAX_RETRIES} retries. Aborting.")
                    exit(1)