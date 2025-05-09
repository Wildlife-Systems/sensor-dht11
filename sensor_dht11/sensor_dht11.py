#!/usr/bin/env python3
import sys
import time
import board
import adafruit_dht
import os
import json

def identify():
    """Identify the sensor."""
    sys.exit(60)

def list_sensors():
    """List available sensors."""
    print("temperature")
    print("humidity")
    sys.exit(0)

def read_sensor():
    """Read data from the DHT11 sensor."""
    dhtDevice = adafruit_dht.DHT11(board.D4)

    temperature = {
        "sensor": "dht11_temperature",
        "measures": "temperature",
        "unit": "Celsius"
    }

    humidity = {
        "sensor": "dht11_humidity",
        "measures": "humidity",
        "unit": "percentage"
    }

    while True:
        try:
            temperature["value"] = dhtDevice.temperature
            humidity["value"] = dhtDevice.humidity
            dhtDevice.exit()
            break
        except RuntimeError as error:
            print(error.args[0])
            time.sleep(2.0)
            continue
        except Exception as error:
            dhtDevice.exit()
            raise error

    return [temperature, humidity]

if __name__ == "__main__":
    if len(sys.argv) == 2:
        if sys.argv[1] == "identify":
            identify()
        elif sys.argv[1] == "list":
            list_sensors()

    data = read_sensor()
    print(json.dumps(data))
