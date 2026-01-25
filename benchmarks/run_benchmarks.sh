#!/bin/bash
# Run DHT11 benchmarks - compares C vs Python implementations
# Usage: ./run_benchmarks.sh [count]
# Note: Will use sudo for GPIO access

set -e

COUNT=${1:-500}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Temporary venv for Python benchmark
TEMP_VENV="$SCRIPT_DIR/.bench_venv"

echo "=============================================="
echo "DHT11 Benchmark: C vs Python"
echo "=============================================="
echo "Reads per test: $COUNT"
echo ""

# Clear old result files
rm -f results_c.csv results_python.csv

# Compile C benchmark if needed
if [ ! -f "dht11_bench" ] || [ "dht11_bench.c" -nt "dht11_bench" ]; then
    echo "Compiling C benchmark..."
    gcc -Wall -Wextra -O2 -std=c99 -o dht11_bench dht11_bench.c -lgpiod
    echo "Done."
    echo ""
fi

# Run C benchmark (with sudo if not already root)
echo "=============================================="
echo "Running C benchmark..."
echo "=============================================="
if [ "$EUID" -eq 0 ]; then
    if ./dht11_bench "$COUNT"; then
        C_RAN=1
    else
        echo "WARNING: C benchmark failed"
        C_RAN=0
    fi
else
    if sudo ./dht11_bench "$COUNT"; then
        C_RAN=1
    else
        echo "WARNING: C benchmark failed"
        C_RAN=0
    fi
fi
echo ""

# Run Python benchmark (needs sudo for GPIO)
echo "=============================================="
echo "Running Python benchmark..."
echo "=============================================="

# Create temporary venv
echo "Creating temporary Python venv..."
python3 -m venv "$TEMP_VENV"
PYTHON_CMD="$TEMP_VENV/bin/python3"
PIP_CMD="$TEMP_VENV/bin/pip3"

# Install required packages
echo "Installing required Python packages..."
"$PIP_CMD" install --quiet RPi.GPIO adafruit-circuitpython-dht adafruit-blinka

# Run Python with sudo
if [ "$EUID" -eq 0 ]; then
    if "$PYTHON_CMD" benchmark_python.py "$COUNT"; then
        PY_RAN=1
    else
        echo "WARNING: Python benchmark failed or was skipped"
        PY_RAN=0
    fi
else
    if sudo "$PYTHON_CMD" benchmark_python.py "$COUNT"; then
        PY_RAN=1
    else
        echo "WARNING: Python benchmark failed or was skipped"
        PY_RAN=0
    fi
fi

# Cleanup temporary venv
echo "Cleaning up temporary venv..."
rm -rf "$TEMP_VENV"
echo ""

# Compare results if both files exist
if [ -f "results_c.csv" ] && [ -f "results_python.csv" ]; then
    echo "=============================================="
    echo "Comparison: C vs Python"
    echo "=============================================="
    echo ""
    
    # Calculate C statistics
    C_AVG=$(awk -F',' 'NR>1 {sum+=$2; count++} END {printf "%.4f", sum/count}' results_c.csv)
    C_MIN=$(awk -F',' 'NR>1 {if(NR==2 || $2<min) min=$2} END {printf "%.4f", min}' results_c.csv)
    C_MAX=$(awk -F',' 'NR>1 {if($2>max) max=$2} END {printf "%.4f", max}' results_c.csv)
    C_SUCCESS=$(awk -F',' 'NR>1 && $3>0 {count++} END {print count+0}' results_c.csv)
    C_TOTAL=$(awk -F',' 'NR>1 {count++} END {print count}' results_c.csv)
    C_ATTEMPTS=$(awk -F',' 'NR>1 && $3>0 {sum+=$3; count++} END {printf "%.2f", sum/count}' results_c.csv)
    
    # Calculate Python statistics
    PY_AVG=$(awk -F',' 'NR>1 {sum+=$2; count++} END {printf "%.4f", sum/count}' results_python.csv)
    PY_MIN=$(awk -F',' 'NR>1 {if(NR==2 || $2<min) min=$2} END {printf "%.4f", min}' results_python.csv)
    PY_MAX=$(awk -F',' 'NR>1 {if($2>max) max=$2} END {printf "%.4f", max}' results_python.csv)
    PY_SUCCESS=$(awk -F',' 'NR>1 && $3>0 {count++} END {print count+0}' results_python.csv)
    PY_TOTAL=$(awk -F',' 'NR>1 {count++} END {print count}' results_python.csv)
    PY_ATTEMPTS=$(awk -F',' 'NR>1 && $3>0 {sum+=$3; count++} END {printf "%.2f", sum/count}' results_python.csv)
    
    # Calculate differences
    SPEEDUP=$(echo "scale=2; $PY_AVG / $C_AVG" | bc -l 2>/dev/null || echo "N/A")
    TIME_SAVED=$(echo "scale=4; $PY_AVG - $C_AVG" | bc -l 2>/dev/null || echo "N/A")
    
    printf "%-20s %12s %12s %12s\n" "Metric" "C" "Python" "Difference"
    printf "%-20s %12s %12s %12s\n" "--------------------" "------------" "------------" "------------"
    printf "%-20s %11ss %11ss %11ss\n" "Avg time/read" "$C_AVG" "$PY_AVG" "$TIME_SAVED"
    printf "%-20s %11ss %11ss\n" "Min time" "$C_MIN" "$PY_MIN"
    printf "%-20s %11ss %11ss\n" "Max time" "$C_MAX" "$PY_MAX"
    printf "%-20s %8s/%s %8s/%s\n" "Success rate" "$C_SUCCESS" "$C_TOTAL" "$PY_SUCCESS" "$PY_TOTAL"
    printf "%-20s %12s %12s\n" "Avg attempts" "$C_ATTEMPTS" "$PY_ATTEMPTS"
    echo ""
    echo "C is ${SPEEDUP}x faster than Python (saves ${TIME_SAVED}s per read)"
    echo ""
    echo "Results saved to:"
    echo "  C:      results_c.csv"
    echo "  Python: results_python.csv"
fi

echo ""
echo "Benchmark complete!"
