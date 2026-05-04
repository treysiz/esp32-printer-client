import asyncio
import websockets
import json
import time

clients = set()

async def handler(websocket):
    print(f"[+] New connection from {websocket.remote_address}")
    clients.add(websocket)
    try:
        async for message in websocket:
            print(f"[<] Received: {message}")
            try:
                data = json.loads(message)
                if data.get("type") == "register":
                    print(f"    --> Device Registered: {data.get('device_id')} from {data.get('store_id')}")
                elif data.get("type") == "print_result":
                    print(f"    --> Print Result: {data.get('order_id')} = {data.get('status')}")
                    if data.get("reason"):
                        print(f"        Reason: {data.get('reason')}")
            except json.JSONDecodeError:
                print("    --> Invalid JSON received")
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        print(f"[-] Connection closed from {websocket.remote_address}")
        clients.remove(websocket)

async def interactive_prompt():
    order_id_counter = 1000
    while True:
        # Run input in an executor to avoid blocking the asyncio loop
        cmd = await asyncio.get_event_loop().run_in_executor(None, input, "Press Enter to send test order (or 'q' to quit): ")
        if cmd.lower() == 'q':
            # Stop the loop or exit gracefully
            import os
            os._exit(0)
        
        if not clients:
            print("No clients connected. Cannot send order.")
            continue
            
        order_id = str(order_id_counter)
        order_id_counter += 1
        
        payload = {
            "type": "print",
            "order_id": order_id,
            "content": f"***************\n TEST ORDER {order_id}\n***************\nItem A x1\nItem B x2\n----------------\nThank you!\n\n\n\n"
        }
        
        msg = json.dumps(payload)
        print(f"[>] Sending to {len(clients)} clients: {msg}")
        
        # Broadcast
        for client in clients:
            try:
                await client.send(msg)
            except Exception as e:
                print(f"Failed to send to client: {e}")

async def main():
    print("Starting WebSocket Server on ws://0.0.0.0:3001/printer")
    server = await websockets.serve(handler, "0.0.0.0", 3001)
    
    # Run the interactive prompt alongside the server
    await asyncio.gather(
        server.wait_closed(),
        interactive_prompt()
    )

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Server stopped.")
