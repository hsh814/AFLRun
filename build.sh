#!/usr/bin/bash

mkdir -p thirdparty/install
export PATH_TO_INSTALL="$PWD/thirdparty/install"
pushd thirdparty
  # Clone LLVM project.
  git clone --depth=1 https://github.com/llvm/llvm-project.git
  pushd llvm-project
    cd llvm-project
    git fetch origin --depth=1 4a2c05b05ed07f1f620e94f6524a8b4b2760a0b1 && \
    git reset --hard 4a2c05b05ed07f1f620e94f6524a8b4b2760a0b1

    # Download binutils.
    wget https://ftp.gnu.org/gnu/binutils/binutils-2.39.tar.gz -O binutils.tar.gz && \
      tar -xf binutils.tar.gz

    # Download CMake.
    wget https://github.com/Kitware/CMake/releases/download/v3.25.1/cmake-3.25.1-linux-x86_64.tar.gz -O cmake.tar.gz && \
      tar -xf cmake.tar.gz

    mkdir build
    pushd build
      export CXX=g++
      export CC=gcc
      ../cmake-3.25.1-linux-x86_64/bin/cmake \
        -DLLVM_BINUTILS_INCDIR=$PWD/../binutils-2.39/include \
        -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=host \
        -DLLVM_ENABLE_PROJECTS="clang;compiler-rt;lld" \
        -DCMAKE_INSTALL_PREFIX="$PATH_TO_INSTALL" \
        -DLLVM_INSTALL_BINUTILS_SYMLINKS=ON $PWD/../llvm/
      make -j $(nproc)
      make install
    popd
  popd
popd
export PATH="$PATH_TO_INSTALL/bin:$PATH"
export LD_LIBRARY_PATH="$PATH_TO_INSTALL/lib:$LD_LIBRARY_PATH"
CC=clang CXX=clang++ make clean all -j 32