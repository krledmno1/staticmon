#!/bin/bash
# Generate a Conan 2 profile and a configure.sh for the host toolchain.
PROJECT_MODE=Debug
read -p "Compiler (G)CC or (C)lang? " choice
case "$choice" in
  g|G )
    CC=gcc
    CXX=g++
    CONAN_COMPILER=gcc ;;
  c|C )
    CC=clang
    CXX=clang++
    CONAN_COMPILER=clang ;;
  * )
    echo "invalid input"
    exit 1 ;;
esac

read -p "Compiler version (Major)? " COMPILER_VERSION

read -p "Build in release mode (y/n)? " choice
case "$choice" in
  y|Y )
    PROJECT_MODE=RelWithDebInfo
    SANITIZERS=OFF
    JEMALLOC=ON ;;
  n|N )
    PROJECT_MODE=Debug
    SANITIZERS=ON
    JEMALLOC=OFF ;;
  * )
    echo "invalid input"
    exit 1 ;;
esac

# Conan 2 os/arch/libcxx from the host.
case "$(uname -m)" in
  x86_64|amd64)  CONAN_ARCH=x86_64 ;;
  arm64|aarch64) CONAN_ARCH=armv8 ;;
  *)             CONAN_ARCH=$(uname -m) ;;
esac
case "$(uname -s)" in
  Darwin) CONAN_OS=Macos; LIBCXX=libc++ ;;
  *)      CONAN_OS=Linux; LIBCXX=libstdc++11 ;;
esac
# On macOS the system "clang" is Apple's fork (Conan calls it apple-clang).
if [ "$CONAN_OS" = "Macos" ] && [ "$CC" = "clang" ]; then
  CONAN_COMPILER=apple-clang
fi

cat > profile <<- EOM
[settings]
os=${CONAN_OS}
arch=${CONAN_ARCH}
compiler=${CONAN_COMPILER}
compiler.libcxx=${LIBCXX}
compiler.cppstd=20
compiler.version=${COMPILER_VERSION}
build_type=${PROJECT_MODE}
EOM

cat > configure.sh <<- EOM
#!/bin/bash
rm -rf builddir
conan install . --output-folder=builddir --build=missing -pr:h=profile -pr:b=profile
CXX=${CXX} CC=${CC} cmake -G Ninja \\
  -DCMAKE_TOOLCHAIN_FILE="\${PWD}/builddir/conan_toolchain.cmake" \\
  -DCMAKE_INSTALL_PREFIX="\${PWD}/install" \\
  -DCMAKE_BUILD_TYPE=${PROJECT_MODE} \\
  -DUSE_JEMALLOC=${JEMALLOC} -DENABLE_SANITIZERS=${SANITIZERS} \\
  -S . -B builddir
EOM

chmod +x configure.sh
echo "Run ./configure.sh to set up the project, then: ninja -C builddir"
