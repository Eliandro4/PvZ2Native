#!/bin/bash

set -e

cd "$(dirname "$0")"

if [ ! -d build ]; then
    echo "Created build"
    mkdir build
fi
cd build
cmake \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBOOST_ROOT="$(dirname "$0")/third_party/boost_1_84_0" \
    -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF ..
cmake --build .
cd ..