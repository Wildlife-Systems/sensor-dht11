# sensor-dht11

sensor-dht11 reads DHT11 temperature and humidity sensors connected to the GPIO
pins of a Raspberry Pi and outputs their readings in the WildlifeSystems
format. It is written in C and reads the pins through the Linux GPIO character
device.

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

[Add the WildlifeSystems APT repository to the system](https://wildlife.systems/apt-configuration.html), then install the package.

```bash
sudo apt update
sudo apt install sensor-dht11
```

## Usage

### Read sensors

Not specifying a command reads both measurements from every sensor.

```bash
# Read all sensors (temperature and humidity)
sensor-dht11

# Read all sensors explicitly
sensor-dht11 all

# Read only temperature
sensor-dht11 temperature

# Read only humidity
sensor-dht11 humidity

# Read only internal, or only external, sensors
sensor-dht11 internal
sensor-dht11 external

# List the available measurements
sensor-dht11 list

# Identify (exits with code 60)
sensor-dht11 identify

# Show the version
sensor-dht11 version

# Output mock readings for testing
sensor-dht11 mock
```

The `setup` and `enable` commands report that nothing is required, since the
sensor is read by bit-banging a GPIO pin and no device-tree overlay has to be
enabled. They are provided so that every WildlifeSystems driver answers the
same commands.

## Configuration

Configuration is read from `/etc/ws/sensors/dht11.json`. Without the file, a
single sensor on GPIO 4 is read.

```json
[
  {
    "pin": 4,
    "internal": false,
    "location": "{{node}}"
  }
]
```

### Configuration options

- `pin`: the GPIO pin number, 2 to 27. The default is 4.
- `internal`: whether the sensor is inside the enclosure. The default is
  false.
- `sensor_id`: a custom sensor identifier. If omitted, the identifier is
  `<serial>_dht11`, where `<serial>` is the Raspberry Pi serial number. Where
  more than one entry omits it, each is instead `<serial>_dht11_pin<N>`, so
  that two sensors do not share an identifier. Each measurement appends its
  own name, e.g. `<serial>_dht11_temperature`.
- `sensor_name`: a human-readable name, reported in the `sensor_name` field.
- `location`: where the sensor is. Either `"{{node}}"` for the position of the
  node, `"{{none}}"` for a sensor that has no position, or an object with
  `latitude` and `longitude` in decimal degrees and, optionally, `altitude`
  and `accuracy` in metres. If omitted, the `location` field of the reading is
  null.

An example showing every option in use is installed as
`/usr/share/doc/sensor-dht11/examples/dht11.json`; it is valid JSON and can be
copied into place and edited.

## Output

The program outputs a JSON array in the WildlifeSystems format with one reading
per measurement. The `node_id` and `deployment_id` fields are filled in by
`sr`.

```json
[
  {"sensor":"dht11_temperature","device":"dht11","measures":"temperature","value":23.0,"unit":"Celsius","node_id":null,"sensor_id":"1234567890abcdef_dht11_temperature","sensor_name":null,"location":null,"deployment_id":null,"timestamp":1789225958,"config":null,"internal":false,"error":null},
  {"sensor":"dht11_humidity","device":"dht11","measures":"humidity","value":45.0,"unit":"percentage","node_id":null,"sensor_id":"1234567890abcdef_dht11_humidity","sensor_name":null,"location":null,"deployment_id":null,"timestamp":1789225958,"config":null,"internal":false,"error":null}
]
```

A reading that could not be taken has a null `value` and the reason in
`error`.

## How it works

The DHT11 is read by bit-banging a GPIO pin from user space, through the Linux
GPIO character device `/dev/gpiochip0`. Version 2 of that interface is used, so
Linux 5.10 or later is required, and no GPIO library is needed. The pulses that
carry the data are between 26 and 70 microseconds long, so the read is performed
under SCHED_FIFO real-time scheduling to reduce the timing failures caused by
pre-emption. The `cap_sys_nice` capability, which allows this without root, is
set on the binary when the package is installed.

A failed read is retried with increasing delays, within a budget of 8 seconds
per invocation shared between the configured sensors. The budget keeps the
program within the 10 seconds that `sr` allows, so that a sensor which cannot
be read is reported with an error rather than the program being terminated
before it can report anything.

## Exit codes

- `0`: success
- `20`: an invalid argument, or the readings could not be produced at all (for
  example, `sc-prototype` was unavailable). Nothing is printed in that case.
- `60`: the `identify` command

## Documentation

[Performance comparison between C and previous Python versions](https://reports.ebaker.me.uk/WS-sensor-dht11.html)

## Author

Ed Baker <ed@ebaker.me.uk>

## Project

Part of the WildlifeSystems project. For more information, visit:
- https://wildlife.systems
- https://docs.wildlife.systems
