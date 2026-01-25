#!/usr/bin/env python3
"""
Benchmark script for DHT11 sensor reads using Adafruit library.
Pure sensor read benchmark - equivalent to C benchmark for fair comparison.
Includes retries like C version.
Usage: python3 benchmark_python.py [count]
"""

import sys
import time
import board
import adafruit_dht

# Retry delays in seconds: 0.1s x3, then 0.2, 0.4, 0.8, 1.6, 2s x3
RETRY_DELAYS = [0.1, 0.1, 0.1, 0.2, 0.4, 0.8, 1.6, 2.0, 2.0, 2.0]

# Default count
count = 500
if len(sys.argv) >= 2:
    count = int(sys.argv[1])
    if count <= 0:
        print(f"Usage: {sys.argv[0]} [count]", file=sys.stderr)
        sys.exit(1)

# Initialize DHT11 on GPIO pin 4
dhtDevice = adafruit_dht.DHT11(board.D4)

times = []
attempts_list = []
successes = 0
failures = 0
total_attempts = 0

print(f"Running {count} DHT11 reads on GPIO 4...", file=sys.stderr)

# Open output file
with open("results_python.csv", "w") as f:
    f.write("read,time,attempts\n")
    
    for i in range(count):
        start = time.time()
        temp = None
        humidity = None
        attempts = 0
        
        # Retry loop like C version
        for attempt, delay in enumerate([0] + RETRY_DELAYS):
            if attempt > 0:
                time.sleep(delay)
            
            dhtDevice._last_called = 0
            attempts = attempt + 1
            
            try:
                temp = dhtDevice.temperature
                humidity = dhtDevice.humidity
                break  # Success
            except RuntimeError:
                pass  # Retry
        
        elapsed = time.time() - start
        times.append(elapsed)
        
        if temp is not None:
            successes += 1
            total_attempts += attempts
            attempts_list.append(attempts)
            f.write(f"{i+1},{elapsed:.6f},{attempts}\n")
        else:
            failures += 1
            attempts_list.append(-1)
            f.write(f"{i+1},{elapsed:.6f},-1\n")
        
        # Progress indicator every 50 reads
        if (i + 1) % 50 == 0:
            print(f"  Progress: {i+1}/{count}", file=sys.stderr)
        
        # Short delay between readings (same as C benchmark)
        if i < count - 1:
            time.sleep(0.1)

dhtDevice.exit()

# Calculate statistics
avg_time = sum(times) / len(times)
min_time = min(times)
max_time = max(times)
total_time = sum(times)

# Print summary to stdout
print(f"\n=== Python Statistics ===")
print(f"Readings:     {successes} success, {failures} failed ({successes/count*100:.1f}% success rate)")
if successes > 0:
    print(f"Avg attempts: {total_attempts/successes:.2f} per successful read")
print(f"Timing:       min={min_time:.4f}s, max={max_time:.4f}s, avg={avg_time:.4f}s, total={total_time:.4f}s")
print(f"Results saved to results_python.csv")
