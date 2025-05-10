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
            print("Invalid command. Use 'identify' or 'list'.")
            sys.exit(1)
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

    result = subprocess.run(["bash", "sc-prototype"], capture_output=True, text=True)
    if result.returncode != 0:
        print("Error: Unable to run sc-prototype script.")
        sys.exit(1)
    
    # Read JSON from result
    try:
        json_data = json.loads(result.stdout)
    except json.JSONDecodeError:
        print("Error: Failed to decode JSON data from sc-prototype.")
        sys.exit(1)

    json_data["sensor"] = "dht11"

    temperature = json_data
    temperature["measures"] = "temperature"
    temperature["unit"] = "Celsisu"
    humidity = json_data
    humidity["measures"] = "humidity"
    humidity["unit"] = "percent"

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
