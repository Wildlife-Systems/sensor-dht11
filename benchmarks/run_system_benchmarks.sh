#!/bin/bash
# Full-system benchmark: compares local vs system-installed sensor-dht11
# Usage: ./run_system_benchmarks.sh [count]
# Note: Will use sudo for GPIO access

set -e

COUNT=${1:-100}
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Output files (different from existing benchmarks)
LOCAL_RESULTS="$SCRIPT_DIR/results_local_binary.csv"
SYSTEM_RESULTS="$SCRIPT_DIR/results_system_binary.csv"
SUMMARY_FILE="$SCRIPT_DIR/system_benchmark_summary.txt"

# Local binary path
LOCAL_BINARY="$PROJECT_DIR/sensor-dht11"

# System binary (from PATH)
SYSTEM_BINARY="sensor-dht11"

echo "=============================================="
echo "Full-System Benchmark: Local vs System Binary"
echo "=============================================="
echo "Iterations: $COUNT"
echo "Local binary: $LOCAL_BINARY"
echo "System binary: $(which $SYSTEM_BINARY 2>/dev/null || echo 'not found')"
echo ""

# Check if local binary exists, build if needed
if [ ! -f "$LOCAL_BINARY" ]; then
    echo "Local binary not found, building..."
    cd "$PROJECT_DIR"
    make
    cd "$SCRIPT_DIR"
    echo "Done."
    echo ""
fi

if [ ! -f "$LOCAL_BINARY" ]; then
    echo "ERROR: Could not find or build local binary at $LOCAL_BINARY"
    exit 1
fi

# Check if system binary exists
if ! command -v "$SYSTEM_BINARY" &> /dev/null; then
    echo "WARNING: System binary '$SYSTEM_BINARY' not found in PATH"
    echo "Only running local benchmark."
    RUN_SYSTEM=0
else
    SYSTEM_BINARY_PATH="$(which $SYSTEM_BINARY)"
    echo "System binary found at: $SYSTEM_BINARY_PATH"
    RUN_SYSTEM=1
fi
echo ""

# Clear old result files
rm -f "$LOCAL_RESULTS" "$SYSTEM_RESULTS" "$SUMMARY_FILE"

# Function to run benchmark on a binary
run_benchmark() {
    local binary_path="$1"
    local output_file="$2"
    local label="$3"
    local count="$4"
    
    echo "=============================================="
    echo "Running $label benchmark ($count iterations)..."
    echo "=============================================="
    
    # Write CSV header
    echo "iteration,exit_code,real_time_sec,user_time_sec,sys_time_sec" > "$output_file"
    
    local successes=0
    local failures=0
    local total_real=0
    local total_user=0
    local total_sys=0
    
    for ((i=1; i<=count; i++)); do
        # Use /usr/bin/time for detailed timing (GNU time format)
        # -f format: real user sys (in seconds)
        local time_output
        local exit_code=0
        
        # Create temp file for time output
        local time_file=$(mktemp)
        
        # Run command with timing
        if [ "$EUID" -eq 0 ]; then
            /usr/bin/time -f "%e %U %S" -o "$time_file" "$binary_path" 2>/dev/null || exit_code=$?
        else
            sudo /usr/bin/time -f "%e %U %S" -o "$time_file" "$binary_path" 2>/dev/null || exit_code=$?
        fi
        
        # Parse time output
        read -r real_time user_time sys_time < "$time_file"
        rm -f "$time_file"
        
        # Record result
        echo "$i,$exit_code,$real_time,$user_time,$sys_time" >> "$output_file"
        
        if [ "$exit_code" -eq 0 ]; then
            ((successes++))
            total_real=$(echo "$total_real + $real_time" | bc)
            total_user=$(echo "$total_user + $user_time" | bc)
            total_sys=$(echo "$total_sys + $sys_time" | bc)
        else
            ((failures++))
        fi
        
        # Progress indicator every 10 iterations
        if [ $((i % 10)) -eq 0 ]; then
            echo "  Progress: $i/$count (success: $successes, failed: $failures)"
        fi
        
        # Short delay between runs to avoid sensor overload
        sleep 0.2
    done
    
    echo ""
    echo "$label completed: $successes success, $failures failed"
    echo "Results saved to: $output_file"
    echo ""
    
    # Return stats for summary
    echo "$successes $failures $total_real $total_user $total_sys"
}

# Run local binary benchmark
echo ""
LOCAL_STATS=$(run_benchmark "$LOCAL_BINARY" "$LOCAL_RESULTS" "Local binary" "$COUNT")
read -r LOCAL_SUCCESS LOCAL_FAIL LOCAL_REAL LOCAL_USER LOCAL_SYS <<< "$LOCAL_STATS"

# Run system binary benchmark if available
if [ "$RUN_SYSTEM" -eq 1 ]; then
    echo ""
    SYSTEM_STATS=$(run_benchmark "$SYSTEM_BINARY_PATH" "$SYSTEM_RESULTS" "System binary" "$COUNT")
    read -r SYSTEM_SUCCESS SYSTEM_FAIL SYSTEM_REAL SYSTEM_USER SYSTEM_SYS <<< "$SYSTEM_STATS"
fi

# Generate summary report
echo "=============================================="
echo "Benchmark Summary"
echo "=============================================="
{
    echo "Full-System Benchmark Summary"
    echo "============================="
    echo "Date: $(date)"
    echo "Iterations: $COUNT"
    echo ""
    echo "Local Binary: $LOCAL_BINARY"
    echo "  Success rate: $LOCAL_SUCCESS/$COUNT ($(echo "scale=1; $LOCAL_SUCCESS * 100 / $COUNT" | bc)%)"
    if [ "$LOCAL_SUCCESS" -gt 0 ]; then
        echo "  Avg real time: $(echo "scale=6; $LOCAL_REAL / $LOCAL_SUCCESS" | bc)s"
        echo "  Avg user time: $(echo "scale=6; $LOCAL_USER / $LOCAL_SUCCESS" | bc)s"
        echo "  Avg sys time:  $(echo "scale=6; $LOCAL_SYS / $LOCAL_SUCCESS" | bc)s"
        echo "  Total time:    ${LOCAL_REAL}s"
    fi
    echo ""
    
    if [ "$RUN_SYSTEM" -eq 1 ]; then
        echo "System Binary: $SYSTEM_BINARY_PATH"
        echo "  Success rate: $SYSTEM_SUCCESS/$COUNT ($(echo "scale=1; $SYSTEM_SUCCESS * 100 / $COUNT" | bc)%)"
        if [ "$SYSTEM_SUCCESS" -gt 0 ]; then
            echo "  Avg real time: $(echo "scale=6; $SYSTEM_REAL / $SYSTEM_SUCCESS" | bc)s"
            echo "  Avg user time: $(echo "scale=6; $SYSTEM_USER / $SYSTEM_SUCCESS" | bc)s"
            echo "  Avg sys time:  $(echo "scale=6; $SYSTEM_SYS / $SYSTEM_SUCCESS" | bc)s"
            echo "  Total time:    ${SYSTEM_REAL}s"
        fi
        echo ""
        
        # Comparison if both ran successfully
        if [ "$LOCAL_SUCCESS" -gt 0 ] && [ "$SYSTEM_SUCCESS" -gt 0 ]; then
            LOCAL_AVG=$(echo "scale=6; $LOCAL_REAL / $LOCAL_SUCCESS" | bc)
            SYSTEM_AVG=$(echo "scale=6; $SYSTEM_REAL / $SYSTEM_SUCCESS" | bc)
            
            if [ "$(echo "$LOCAL_AVG < $SYSTEM_AVG" | bc)" -eq 1 ]; then
                SPEEDUP=$(echo "scale=2; $SYSTEM_AVG / $LOCAL_AVG" | bc)
                echo "Comparison: Local binary is ${SPEEDUP}x faster"
            else
                SPEEDUP=$(echo "scale=2; $LOCAL_AVG / $SYSTEM_AVG" | bc)
                echo "Comparison: System binary is ${SPEEDUP}x faster"
            fi
        fi
    else
        echo "System Binary: Not available (not installed or not in PATH)"
    fi
} | tee "$SUMMARY_FILE"

echo ""
echo "Detailed results saved to:"
echo "  Local:   $LOCAL_RESULTS"
if [ "$RUN_SYSTEM" -eq 1 ]; then
    echo "  System:  $SYSTEM_RESULTS"
fi
echo "  Summary: $SUMMARY_FILE"
