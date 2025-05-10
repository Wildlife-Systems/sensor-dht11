from setuptools import setup, find_packages

setup(
    name="sensor-dht11",
    version="1.0.0",
    description="A Python package for reading data from the DHT11 sensor.",
    author="Your Name",
    author_email="your.email@example.com",
    url="https://github.com/yourusername/sensor-dht11",
    packages=find_packages(),
    install_requires=[
        "adafruit-circuitpython-dht",
        "board",
        "RPi.GPIO"
    ],
    entry_points={
        "console_scripts": [
            "sensor-dht11=sensor_dht11.sensor:cli",
        ],
    },
    classifiers=[
        "Programming Language :: Python :: 3",
        "Operating System :: OS Independent",
    ],
    python_requires=">=3.6",
)