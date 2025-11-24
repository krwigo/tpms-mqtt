#!/bin/bash

if ! command -v clang-format >/dev/null 2>&1
then
  apt install -y clang-format
fi

set -x

clang-format -i main/main.c

# clean
idf.py build
# flash monitor
