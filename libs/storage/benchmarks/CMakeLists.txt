#
# F E T C H   S T O R A G E   B E N C H M A R K S
#
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(fetch-storage)

# CMake configuration
include(${FETCH_ROOT_CMAKE_DIR}/BuildTools.cmake)

# Compiler Configuration
setup_compiler()

add_fetch_gbench(stack_benchmarks fetch-storage ./stack_benchmarks)
add_fetch_gbench(transaction_throughput fetch-storage ./transaction_throughput)
