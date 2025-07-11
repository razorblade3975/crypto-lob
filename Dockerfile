###############################################################################
# crypto-lob – Clang-17 + libc++ + Conan 2 dev container
###############################################################################
FROM ubuntu:22.04 AS build-env

ARG CMAKE_BUILD_TYPE=Release
ENV DEBIAN_FRONTEND=noninteractive \
    CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}

# ─────────────────────────────────────────────────────────────────────────────
# 1) LLVM repo → Clang-17 + core build deps
# ─────────────────────────────────────────────────────────────────────────────
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      wget ca-certificates gnupg lsb-release software-properties-common \
 && wget https://apt.llvm.org/llvm.sh \
 && chmod +x llvm.sh \
 && ./llvm.sh 17 \
 && rm llvm.sh \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      clang-17 clang++-17 libc++-17-dev libc++abi-17-dev \
      cmake python3-pip pkg-config libnuma-dev git \
 && rm -rf /var/lib/apt/lists/* \
 && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-17 100 \
 && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-17 100

# ─────────────────────────────────────────────────────────────────────────────
# 2) Conan 2 – detect profile **after** exporting Clang env
# ─────────────────────────────────────────────────────────────────────────────
ENV CC=clang \
    CXX=clang++ \
    CMAKE_GENERATOR=Ninja

RUN pip3 install --no-cache-dir "conan>=2.0,<3" \
 && conan profile detect --force \
 && PROFILE=$(conan profile path default) \
 && sed -i '/^compiler\.cppstd=/d;/^compiler\.libcxx=/d' "$PROFILE" \
 && printf 'compiler.cppstd=20\ncompiler.libcxx=libc++\n' >> "$PROFILE"

WORKDIR /workspace
COPY conanfile.* CMakeLists.txt ./
RUN mkdir build

###############################################################################
FROM build-env AS dev

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ninja-build make \
      gdb valgrind linux-tools-generic strace htop vim \
      clang-format-17 clang-tidy-17 \
 && rm -rf /var/lib/apt/lists/*

RUN echo 'vm.nr_hugepages=1024' > /etc/sysctl.d/99-hugepages.conf

COPY --chown=root:root . .
WORKDIR /workspace

EXPOSE 8080 2345
CMD ["/bin/bash"]