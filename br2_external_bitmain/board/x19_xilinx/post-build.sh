#!/bin/sh

set -e

TARGET_DIR=$1

echo "X19 XILINX: Running post-build script..."

# Create essential directories
mkdir -p ${TARGET_DIR}/etc
mkdir -p ${TARGET_DIR}/proc
mkdir -p ${TARGET_DIR}/sys
mkdir -p ${TARGET_DIR}/tmp
mkdir -p ${TARGET_DIR}/root

echo "X19 XILINX: Post-build complete"
