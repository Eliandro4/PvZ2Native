@echo off

cd /D "%~dp0"

if not exist build (
    echo "Created build"
    mkdir build
)
cd build
cmake ^
 -G "MinGW Makefiles" ^
 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
 -DCMAKE_BUILD_TYPE=Release ^
 -DCMAKE_C_COMPILER=gcc ^
 -DCMAKE_CXX_COMPILER=g++ ^
 -DCMAKE_MAKE_PROGRAM=make ^
 -DBOOST_ROOT="%~dp0third_party\boost_1_84_0" ^
 -DDYNARMIC_USE_PRECOMPILED_HEADERS=OFF ^
 ..
cmake --build .
cd ..
