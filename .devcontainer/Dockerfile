FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -y --no-install-recommends \
    software-properties-common \
    wget \
    xz-utils \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt update && apt install -y --no-install-recommends \
    cmake \
    ninja-build \
    gcc-15 \
    g++-15 \
    gdb \
    clang-tidy-20 \
    openocd \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-15 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-15 100 \
    && apt clean \
    && rm -rf /var/lib/apt/lists/*

RUN wget -qO- https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz | tar -xJ -C /opt
ENV PATH="/opt/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin:${PATH}"

RUN usermod -aG dialout vscode

WORKDIR /workspace

CMD ["/bin/bash"]
