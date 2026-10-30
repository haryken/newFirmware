# EMO-Bridge firmware — top-level CMake
# Build:  idf.py set-target esp32 && idf.py build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(emo_bridge)

# Motion files: spiffs_image/mot/*.mot -> "storage" partition (partitions.csv),
# mounted at /spiffs by main/legs.c. Built and flashed with `idf.py flash`
# (or alone with `idf.py storage-flash`).
spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)
