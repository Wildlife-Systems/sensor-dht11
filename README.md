# sensor-dht11

A lightweight C implementation for reading DHT11 temperature and humidity sensors on Raspberry Pi. 

## Building from source

```bash
make
```

## Installing

### From source

```bash
sudo make install
```

### From Debian package

[Add the WildlifeSystems APT repository to your system](https://wildlife.systems/apt-configuration.html)

```bash
sudo apt update
sudo apt install sensor-dht11
```

## Usage

### Read sensors

```bash
# Read all sensors (temperature and humidity)
sensor-dht11

# Read all sensors explicitly
sensor-dht11 all

# Read only temperature
sensor-dht11 temperature

# Read only humidity
sensor-dht11 humidity

# Read only internal sensors
sensor-dht11 internal

# Read only external sensors
sensor-dht11 external

# List available sensor types
sensor-dht11 list

# Identify (exits with code 60)
sensor-dht11 identify

# Show version
sensor-dht11 version

# Output mock data for testing
sensor-dht11 mock
```

## Configuration

Configuration is read from `/etc/ws/sensors/dht11.json`. Example:

```json
[
  {
    "pin": 4,
    "internal": false
  }
]
```

### Configuration options

- `pin`: GPIO pin number (2-27)
- `internal`: Boolean indicating if sensor is inside the enclosure
- `sensor_id`: Optional custom sensor ID (defaults to Pi serial + "_dht11")

## Output

The program outputs JSON in the WildlifeSystems sensor format:

```json
[
  {"sensor":"dht11_temperature","measures":"temperature","unit":"Celsius","value":23.0,"internal":false,"sensor_id":"1234567890abcdef_dht11_temperature"},
  {"sensor":"dht11_humidity","measures":"humidity","unit":"percentage","value":45.0,"internal":false,"sensor_id":"1234567890abcdef_dht11_humidity"}
]
```

## How it works

This program uses a userspace C implementation to read DHT11 sensors via GPIO using the libgpiod library. All timing-critical bit-banging is handled in userspace, not by the kernel.

## Exit codes

- `0`: Success
- `20`: Invalid argument
- `60`: Identify command

## Documentation

[Performance comparison between C and previous Python versions](https://reports.ebaker.me.uk/WS-sensor-dht11.html)

## Author

Ed Baker <ed@ebaker.me.uk>

## Project

Part of the WildlifeSystems project. For more information, visit:
- https://wildlife.systems
- https://docs.wildlife.systems
