#!/usr/bin/python3

import sys
import time
import board
import adafruit_dht
import os
import json

def cli():
    """Command line interface for the DHT11 sensor."""
    if len(sys.argv) == 2:
        if sys.argv[1] == "identify":
            identify()
        elif sys.argv[1] == "list":
            list_sensors()
        else:
            read_sensor()
            sys.exit(0)

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

    # Get template JSON response
    stream = os.popen('sc-prototype')
    output = stream.read()

    temperature = json.loads(output)
    humidity = json.loads(output)

    temperature["sensor"] = "dht11_temperature"
    temperature["measures"] = "temperature"
    temperature["unit"] = "Celsius"

    humidity["sensor"] = "dht11_humidity"
    humidity["measures"] = "humidity"
    humidity["unit"] = "percentage"

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
    if len(sys.argv) == 2:
        if sys.argv[1] =="temperature":
            print("[",json.dumps(temperature),"]")
            return
        elif sys.argv[1] == "humidity":
            print("[",json.dumps(humidity),"]")
            return
        elif sys.argv[1] != "all":
            sys.exit(20)
    print("[", json.dumps(temperature), ",", json.dumps(humidity), "]")
    return

if __name__ == "__main__":
    if len(sys.argv) == 2:
        if sys.argv[1] == "identify":
            identify()
        elif sys.argv[1] == "list":
            list_sensors()

    data = read_sensor()
    print(json.dumps(data))
