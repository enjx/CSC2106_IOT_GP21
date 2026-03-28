# Instructions

## Image Transfer
1. Configure receiver.py and ensure gateway node port is correct (where your receiver/gateway node is connected to)

2. Configure sender.py and ensure sender node port is correct.

3. Configure and upload mesh_node.ino files to LoRa mesh nodes.

4. Ensure serial monitors for receiver and sender nodes are CLOSED.

5. Have an image ready named "image.jpg" (default) or whatever name is configured in the sender.py and reciever.py files.

6. Ensure your current directory has that image file before running.

7. Run receiver.py, then sender.py to transfer the image.

Received image should appear in your current directory.