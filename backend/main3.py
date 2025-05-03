import json
import time
import redis
import logging
import asyncio
import uvicorn
import websockets  # New import for WebSocket client
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from typing import List, Dict
from datetime import datetime
import pytz

logger = logging.getLogger(__name__)

# Redis Configuration
REDIS_HOST = "localhost"
REDIS_PORT = 6379
REDIS_DB = 0

# Initialize Redis
r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=REDIS_DB)

# WebSocket Server for Sending Data to Frontend (Port 8001)
send_app = FastAPI()

# Remote WebSockeg Server URL
REMOTE_WS_URL = "ws://192.168.43.1:8081"  # Change this URL as needed

# CORS Configuration
origins = ["http://192.168.43.144:3000"]
send_app.add_middleware(
    CORSMiddleware,
    allow_origins=origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

class WebSocketManager:
    """Manages WebSocket connections"""
    def __init__(self):
        self.active_connections: List[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        """Accept a new WebSocket connection"""
        await websocket.accept()
        self.active_connections.append(websocket)
        logger.info(f"New WebSocket connection: {websocket.client}")

    def disconnect(self, websocket: WebSocket):
        """Remove a WebSocket connection"""
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)
            logger.info(f"Disconnected: {websocket.client}")

    async def broadcast(self, message: dict):
        """Send data to all connected WebSocket clients"""
        for connection in self.active_connections:
            try:
                await connection.send_json(message)
            except Exception as e:
                logger.error(f"Error sending message to {connection.client}: {e}")

# WebSocket Manager for sending data to the frontend
sender_manager = WebSocketManager()

@send_app.websocket("/ws")
async def websocket_send(websocket: WebSocket):
    """Forwards received sensor data to frontend via WebSocket"""
    await sender_manager.connect(websocket)
    try:
        while True:
            await asyncio.sleep(1)  # Keep connection open
    except WebSocketDisconnect:
        sender_manager.disconnect(websocket)

# Sensor Limits for Warnings
sensor_limits = {
    "E-TC1": 1150, "E-TC2": 1150, "E-TC3": 1150, "E-TC4": 1150, "E-TC5": 1150, "E-TC6": 1150, "E-TC7": 1150, "E-TC8": 1150,
    "E-RTD1": 1150, "E-RTD2": 1150,
    "PT-M1": 990, "PT-M2": 990, "PT-C": 990, "PT-EU": 990, "PT-ED": 990, "PT-L": 990, "PT-P": 990, "PT-FS": 990,
    "R-EMBV": 1000, "R-LMBV": 1000, "R-EVBV": 1000, "R-LVBV": 1000,
    "LC-L": 1000, "LC-E": 1000, "LC-T": 1000
}

def get_sensor_warning(sensor_name, value):
    """Determines if the sensor value exceeds its limit"""
    return 1 if value > sensor_limits.get(sensor_name, 0) else 0

def parse_sensor_data(data: str) -> List[Dict]:
    """Parses incoming WebSocket data and returns standardized sensor data"""
    try:
        parsed = json.loads(data)
        if not isinstance(parsed, list):
            logger.error("Parsed data is not a list.")
            return []

        standardized_data = []
        for sensor in parsed:
            title = sensor.get("title")
            value = sensor.get("value")

            if title is None or value is None:
                logger.error(f"Missing fields in sensor data: {sensor}")
                continue

            try:
                numeric_value = float(value)
            except ValueError:
                logger.error(f"Invalid 'value' for sensor '{title}': {value}")
                continue

            standardized_data.append({
                "name": title,
                "value": numeric_value,
                "timestamp": sensor.get("timestamp")
            })

        return standardized_data

    except json.JSONDecodeError as e:
        logger.error(f"JSON decode error: {e}")
        return []
    except Exception as e:
        logger.error(f"Unexpected error in parse_sensor_data: {e}")
        return []

# New function: Acts as a client connecting to a remote WebSocket server to receive sensor data

async def receive_sensor_data():
    """Connects to a remote WebSocket server and processes incoming sensor data"""
    while True:
        try:
            async with websockets.connect(REMOTE_WS_URL) as websocket:
                logger.info(f"Connected to remote WebSocket server at {REMOTE_WS_URL}")
                buffer = []
                sent_ts = 0
                time_poll = 0

                while True:
                    data = await websocket.recv()  # Receive data as text
                    data_json = parse_sensor_data(data)

                    if not data_json:
                        logger.error(f"Received invalid data: {data}")
                        continue

                    time_ms = int(time.time() * 1000)
                    current_poll = time.time_ns() // 1_000_000
                    if time_poll != current_poll:
                        time_poll = current_poll
                        for sensor_data in data_json:
                            sensor_name = sensor_data.get("name")
                            value = sensor_data.get("value")
                            timestamp = sensor_data.get("timestamp") or time_ms
                            sensor_warning = get_sensor_warning(sensor_name, value)

                            if sensor_name and value is not None:
                                try:
                                    r.execute_command('TS.ADD', sensor_name, timestamp, value)
                                    buffer.append({
                                        "name": sensor_name,
                                        "value": value,
                                        "timestamp": timestamp,
                                        "warning": sensor_warning
                                    })
                                except Exception as e:
                                    logger.error(f"Error adding data to Redis for {sensor_name}: {e}")
                                    continue
                            else:
                                logger.error(f"Invalid sensor data: {sensor_data}")
                                continue
                
                    current_time = int(time.time() * 1000)
                    if buffer and (current_time - sent_ts) > 100:
                        sent_ts = current_time
                        await sender_manager.broadcast(buffer)
                        buffer.clear()
        except Exception as e:
            logger.error(f"Error in WebSocket client connection: {e}")
            await asyncio.sleep(5)  # Wait before attempting to reconnect

# Run the WebSocket client for sensor data and the frontend WebSocket server concurrently
async def main():
    config_send = uvicorn.Config(send_app, host="0.0.0.0", port=8001, log_level="info")
    server_send = uvicorn.Server(config_send)

    await asyncio.gather(
        receive_sensor_data(),  # Run the sensor data receiver as a client
        server_send.serve()     # Run the FastAPI server for sending data to the frontend
    )

if __name__ == "__main__":
    asyncio.run(main())