# from typing import List, Dict, Optional
# import json
# import time
# import redis
# import logging
# from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
# from fastapi.middleware.cors import CORSMiddleware
# from typing import List
# import asyncio
# from datetime import datetime
# import pytz

# logger = logging.getLogger(__name__)

# class ConnectionManager:
#     def __init__(self):
#         self.active_connections: List[WebSocket] = []

#     async def connect(self, websocket: WebSocket):
#         await websocket.accept()
#         self.active_connections.append(websocket)
#         logger.info(f"New connection: {websocket.client}")

#     def disconnect(self, websocket: WebSocket):
#         self.active_connections.remove(websocket)
#         logger.info(f"Disconnected: {websocket.client}")

#     async def broadcast(self, message: dict):
#         # logger.info(f"Broadcasting message: {message}")
#         for connection in self.active_connections:
#             try:
#                 await connection.send_json(message)
#             except Exception as e:
#                 logger.error(f"Error sending message to {connection.client}: {e}")


# # Configuration
# REDIS_HOST = 'localhost'
# REDIS_PORT = 6379
# REDIS_DB = 0
# RETENTION_MS = 3600000  # 1 hour in milliseconds

# # Initialize Redis client
# r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=REDIS_DB)

# def initialize_redis(sensor_names):
#     logger.info("Initializing Redis TimeSeries for sensors.")
#     r.flushdb()
#     for sensor in sensor_names:
#         try:
#             r.execute_command('TS.CREATE', sensor, 'RETENTION', RETENTION_MS, 'IFNOTEXISTS')
#         except redis.exceptions.ResponseError as e:
#             if "TSDB: key already exists" not in str(e):
#                 logger.error(f"Unexpected error while creating time series for {sensor}: {e}")
#             else:
#                 logger.info(f"Time series for {sensor} already exists.")
#     logger.info("Redis TimeSeries initialization complete.")


# # Logger configuration
# logging.basicConfig(level=logging.INFO)
# logger = logging.getLogger("main")

# app = FastAPI()

# # CORS configuration
# origins = ["http://192.168.112.165:3000"]  # Update with your frontend URL
# app.add_middleware(
#     CORSMiddleware,
#     allow_origins=origins,
#     allow_credentials=True,
#     allow_methods=["*"],
#     allow_headers=["*"],
# )

# # Sensor names
# sensor_names = [
#     "E-TC1", "E-TC2", "E-TC3", "E-TC4",
#     "E-RTD1", "E-RTD2", "E-RTD3", "E-RTD4", "E-RTD5", "E-RTD6",
#     "E-PT1", "E-PT2", "E-PT3", "E-PT4", "E-PT5", "E-PTCC", "FS-LPT", "FS-HPT",
#     "E-EMBV", "E-LMBV", "E-EVBV", "E-LVBV", "FS-LBV", "FS-HBV1", "FS-HBV2", "FS-HBV3"
# ]

# sensor_limits = {
#  'E-TC1': 1150,
#  'E-TC2': 1150,
#  'E-TC3': 1150,
#  'E-TC4': 1150,
#  'E-RTD1': 1150,
#  'E-RTD2': 1150,
#  'E-RTD3': 1150,
#  'E-RTD4': 1150,
#  'E-RTD5': 1150,
#  'E-RTD6': 1150,
#  'E-PT1': 990,
#  'E-PT2': 990,
#  'E-PT3': 990,
#  'E-PT4': 990,
#  'E-PT5': 990,
#  'E-PTCC': 990,
#  'FS-LPT': 990,
#  'FS-HPT': 990,
#  'E-EMBV': 1000,
#  'E-LMBV': 1000,
#  'E-EVBV': 1000,
#  'E-LVBV': 1000,
#  'FS-LBV': 1000,
#  'FS-HBV1': 1000,
#  'FS-HBV2': 1000,
#  'FS-HBV3': 1000
# }


# # Initialize Redis on startup
# @app.on_event("startup")
# async def on_startup():
#     initialize_redis(sensor_names)

# # Initialize Connection Manager
# manager = ConnectionManager()

# @app.get("/")
# async def root():
#     return {"message": "Server is running"}


# @app.websocket("/ws")
# async def websocket_endpoint(websocket: WebSocket):
#     logger.info(f"New WebSocket connection")
#     await manager.connect(websocket)
#     sent_ts = 0
#     buffer = []
#     try:
#         time_poll = 0
        
#         while True:
#             data = await websocket.receive_text()
#             data_json = parse_sensor_data(data)
#             if not data_json:
#                 logger.error(f"Received invalid data: {data}")
#                 continue

#             # Get the Greece time zone (Athens)
#             greece_tz = pytz.timezone('Europe/Athens')

#             # Get the current time in Greece (localized time)
#             local_time = datetime.now(greece_tz)

#             # Get the current time in nanoseconds (from Unix epoch)
#             time_ns = time.time_ns()  # Current time in nanoseconds (Unix epoch)

#             # To store nanoseconds in a precise format, get seconds and nanoseconds
#             seconds = time_ns // 1_000_000_000  # Seconds part
#             nanoseconds = time_ns % 1_000_000_000  # Nanoseconds part

#             # Optionally, you can create a complete timestamp with nanosecond precision
#             time_ns = seconds * 1_000_000_000 + nanoseconds  # Combine seconds and nanoseconds into a full timestamp
#             time_ms = time_ns // 1_000_000  # Convert to milliseconds

#             # print(time_ms)
#             if time_poll != time.time_ns() // 1_000_000:
#                 time_poll = time.time_ns() // 1_000_000
                
#                 for sensor_data in data_json:
#                     sensor_name = sensor_data.get("name")
#                     value = sensor_data.get("value")
#                     timestamp = sensor_data.get("timestamp") or time_ms
#                     sensor_warning = get_sensor_warning(sensor_name, value)
                    

#                     if sensor_name and value is not None:
#                         try:
#                             r.execute_command('TS.ADD', sensor_name, timestamp, value)
#                             buffer.append({
#                                 "name": sensor_name,
#                                 "value": value,
#                                 "timestamp": timestamp,
#                                 "warning": sensor_warning
#                             })
#                         except Exception as e:
#                             logger.error(f"Error adding data to Redis for {sensor_name}: {e}")
#                             continue
#                     else:
#                         logger.error(f"Invalid sensor data: {sensor_data}")
#                         continue

#             # Throttle sending to once every 50ms
#             current_time = int(time.time() * 1000)
#             if buffer and (current_time - sent_ts) > 50:
#                 sent_ts = current_time
#                 await manager.broadcast(buffer.copy())
#                 buffer.clear()

#     except WebSocketDisconnect:
#         manager.disconnect(websocket)
#         logger.info(f"WebSocket disconnected: {websocket.client}")
#     except Exception as e:
#         manager.disconnect(websocket)
#         logger.error(f"Unexpected error: {e} with {websocket.client}")

# def get_sensor_warning(sensor_name, value):
#     return 1 if value > sensor_limits.get(sensor_name, 0) else 0

# def parse_sensor_data(data: str) -> List[Dict]:
#     """
#     Parses incoming WebSocket data and returns a list of sensor dictionaries with standardized keys.

#     Args:
#         data (str): JSON-formatted string received from the WebSocket.

#     Returns:
#         List[Dict]: List of dictionaries with keys 'name', 'value', and 'timestamp'.
#     """
#     try:
#         parsed = json.loads(data)
        
#         # Ensure the data is a list
#         if not isinstance(parsed, list):
#             logger.error("Parsed data is not a list.")
#             return []
        
#         standardized_data = []
        
#         for sensor in parsed:
#             # Extract and validate 'title' and 'value'
#             title = sensor.get("title")
#             value = sensor.get("value")
            
#             if title is None:
#                 logger.error(f"Missing 'title' in sensor data: {sensor}")
#                 continue
#             if value is None:
#                 logger.error(f"Missing 'value' in sensor data: {sensor}")
#                 continue
            
#             # Convert 'value' to float if possible
#             try:
#                 numeric_value = float(value)
#             except ValueError:
#                 logger.error(f"Invalid 'value' for sensor '{title}': {value}")
#                 continue
            
#             standardized_data.append({
#                 "name": title,          # Map 'title' to 'name'
#                 "value": numeric_value, # Ensure 'value' is a float
#                 "timestamp": sensor.get("timestamp")  # Optional; can be None
#             })
        
#         return standardized_data
    
#     except json.JSONDecodeError as e:
#         logger.error(f"JSON decode error: {e}")
#         return []
#     except Exception as e:
#         logger.error(f"Unexpected error in parse_sensor_data: {e}")
#         return []


# @app.get("/time_range/")
# async def get_time_range(
#     start_time: Optional[int] = None, 
#     end_time: Optional[int] = None,
#     ts_key: Optional[str] = None
# ):
#     """
#     GET endpoint that queries a Redis TimeSeries for data in the specified time range:
#     - start_time: the start of the time range in milliseconds (int).
#     - end_time: the end of the time range in milliseconds (int).
#     - ts_key: the TimeSeries key in Redis to query.
    
#     This endpoint returns the data within the specified time range.
#     """
#     # Check if the required parameters are provided
#     if not start_time or not end_time:
#         raise HTTPException(status_code=400, detail="Both start_time and end_time must be provided")

#     if not ts_key:
#         raise HTTPException(status_code=400, detail="TimeSeries key (ts_key) must be provided")

#     # Convert ms to seconds for Redis (Redis TimeSeries uses seconds)
#     # start_time_seconds = start_time / 1000
#     # end_time_seconds = end_time / 1000

#     logger.info(f"Querying Redis with ts_key={ts_key}, start_time_seconds={start_time}, end_time_seconds={end_time}")

#     try:
#         # Query Redis TimeSeries data for the given time range and the specific key
#         data = r.ts().range(ts_key, start_time, end_time)

#         # Format the results into a list of dictionaries for easier readability
#         result = [{"timestamp": int(entry[0]), "value": entry[1]} for entry in data]

#         if not result:
#             raise HTTPException(status_code=404, detail="No data found in the specified time range")

#         return {"ts_key": ts_key, "start_time": start_time, "end_time": end_time, "data": result}

#     except redis.exceptions.ConnectionError as e:
#         logger.error(f"Redis connection error: {e}")
#         raise HTTPException(status_code=500, detail="Error connecting to Redis")
#     except redis.exceptions.ResponseError as e:
#         logger.error(f"Redis response error: {e}")
#         raise HTTPException(status_code=500, detail="Error querying Redis TimeSeries")
#     except Exception as e:
#         logger.error(f"Unexpected error: {e}")
#         raise HTTPException(status_code=500, detail=f"Unexpected error: {str(e)}")

import json
import time
import redis
import logging
import asyncio
import uvicorn
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

# WebSocket Server for Receiving Data (Port 8000)
receive_app = FastAPI()

# WebSocket Server for Sending Data to Frontend (Port 8001)
send_app = FastAPI()

# CORS Configuration
origins = ["http://192.168.1.102:3000"]
for app in [receive_app, send_app]:
    app.add_middleware(
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
        self.active_connections.remove(websocket)
        logger.info(f"Disconnected: {websocket.client}")

    async def broadcast(self, message: dict):
        """Send data to all connected WebSocket clients"""
        for connection in self.active_connections:
            try:
                await connection.send_json(message)
            except Exception as e:
                logger.error(f"Error sending message to {connection.client}: {e}")

# WebSocket Managers
receiver_manager = WebSocketManager()  # Receives data on port 8000
sender_manager = WebSocketManager()  # Sends data to frontend (port 8001)

@receive_app.websocket("/ws")
async def websocket_receive(websocket: WebSocket):
    """Receives sensor data via WebSocket"""
    await receiver_manager.connect(websocket)
    buffer = []
    sent_ts = 0
    try:
        time_poll = 0
        while True:
            data = await websocket.receive_text()
            data_json = parse_sensor_data(data)

            if not data_json:
                logger.error(f"Received invalid data: {data}")
                continue

            # Get current timestamp
            time_ms = int(time.time() * 1000)

            if time_poll != time.time_ns() // 1_000_000:
                time_poll = time.time_ns() // 1_000_000
                
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
            # queue_size = len(websocket._buffer)  # Internal buffer of messages
            # logger.info(f"Queue size: {queue_size}")
            # Throttle sending to once every 50ms
            current_time = int(time.time() * 1000)
            if buffer and (current_time - sent_ts) > 500:
                sent_ts = current_time
                await sender_manager.broadcast(buffer)
                buffer.clear()

    except WebSocketDisconnect:
        receiver_manager.disconnect(websocket)

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

# Run both WebSocket servers asynchronously
async def main():
    """Runs both WebSocket servers concurrently"""
    config_receive = uvicorn.Config(receive_app, host="0.0.0.0", port=8000, log_level="info")
    config_send = uvicorn.Config(send_app, host="0.0.0.0", port=8001, log_level="info")

    server_receive = uvicorn.Server(config_receive)
    server_send = uvicorn.Server(config_send)

    await asyncio.gather(
        server_receive.serve(),
        server_send.serve()
    )

if __name__ == "__main__":
    asyncio.run(main())  # Runs both WebSocket servers in the same event loop
