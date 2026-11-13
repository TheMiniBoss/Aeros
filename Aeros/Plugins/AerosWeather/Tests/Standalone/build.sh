#!/usr/bin/env bash
# Build and run the standalone tests (Linux, macOS, WSL, Git Bash with g++ or clang++).
#   ./build.sh            build everything and run the automation tests
#   ./build.sh scenarios  then: ./bin/scenarios supercell
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
SRC=../../Source/AerosWeather
FLAGS="-std=c++20 -O2 -Wall -Wno-unused -Ishim -I$SRC/Public -I$SRC/Private"
CORE="$SRC/Private/Core/AerosSimulation.cpp $SRC/Private/Core/AerosSnapshot.cpp"
mkdir -p bin
$CXX $FLAGS -o bin/run_automation_tests run_automation_tests.cpp $CORE
$CXX $FLAGS -o bin/scenarios scenarios.cpp $CORE
$CXX $FLAGS -o bin/hazards hazards.cpp $CORE
echo "Built bin/run_automation_tests, bin/scenarios, bin/hazards"
if [ "${1:-}" = "" ]; then ./bin/run_automation_tests; fi
