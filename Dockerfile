FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    g++-13 \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc-13
ENV CXX=g++-13

WORKDIR /workspace
