#
# T X   G E N E R A T O R
#
cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(tx-generator)

setup_compiler()

add_executable(tx-generator main.cpp)
target_link_libraries(tx-generator PRIVATE fetch-ledger)
