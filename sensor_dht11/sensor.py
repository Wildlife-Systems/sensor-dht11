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
    sensor_json = ""
    """Read JSON from /etc/ws/dht11.json"""
    if os.path.exists("/etc/ws/dht11.json"):
        #Create empty arrat
        with open("/etc/ws/dht11.json") as f:
            data = json.load(f)
        # Loop over JSON objects in data array
        for sensor in data:
            if "pin" in sensor:
                pin = sensor["pin"]
            else:
                pin = 4
            if "internal" in sensor:
                internal = sensor["internal"]
            else:
                internal = False
            json_string = read_sensor_helper(pin, internal)
            #Merge the JSON arrays in strings sensor_json and json_string
            if sensor_json == "":
                sensor_json = json_string
            else:
                sensor_json = sensor_json[:-1] + "," + json_string[1:]
        return

    else:
        print(read_sensor_helper(4))
        return

def read_sensor_helper(pin, internal=False):
    if pin == 4:
        pin = board.D4
    else:
        print("Invalid pin number")
        sys.exit(21)

    """Read data from the DHT11 sensor."""
    dhtDevice = adafruit_dht.DHT11(pin)

    # Get template JSON response
    stream = os.popen('sc-prototype')
    output = stream.read()

    temperature = json.loads(output)
    humidity = json.loads(output)

    temperature["sensor"] = "dht11_temperature"
    temperature["measures"] = "temperature"
    temperature["unit"] = "Celsius"
    temperature["internal"] = internal

    humidity["sensor"] = "dht11_humidity"
    humidity["measures"] = "humidity"
    humidity["unit"] = "percentage"
    humidity["internal"] = internal

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
        if sys.argv[1] == "temperature":
            ret = "[" + json.dumps(temperature) + "]"
            return ret
        elif sys.argv[1] == "humidity":
            ret = "[" + json.dumps(humidity) + "]"
            return ret
        elif sys.argv[1] != "all":
            sys.exit(20)

    ret = "[" + json.dumps(temperature) + "," + json.dumps(humidity) + "]"
    return ret

if __name__ == "__main__":
    if len(sys.argv) == 2:
        if sys.argv[1] == "identify":
            identify()
        elif sys.argv[1] == "list":
            list_sensors()

    data = read_sensor()
    print(json.dumps(data))
