# Contributing to NUMA Fabric

Thank you for your interest in contributing to NUMA Fabric. This document
describes how to contribute and the terms under which contributions are
accepted. NUMA Fabric is licensed under the Apache License, Version 2.0.

## Contributor License Agreement

There is **no Contributor License Agreement (CLA)**. By submitting a
contribution, you agree that it is made available under the Apache License,
Version 2.0, consistent with the license and NOTICE files in this repository,
and that you have the right to grant the rights described in Apache 2.0
(section 5). Do not add "Co-authored-by" or other trailers that you were not
asked to add.

## How to contribute

1. Fork the repository and create a feature branch.
2. Make focused, small changes. Each change should be accompanied by tests
   and, where it changes behavior, documentation.
3. Ensure the project builds cleanly with the project's compiler flags
   (see below) and that the test suite passes. Do not weaken assertions to
   make tests pass; fix the underlying defect instead.
4. Keep the public API coherent and vendor-neutral. The runtime must talk to
   the machine only through the backend::Backend interface.
5. Do not add telemetry or any external transmission of usage data.

## Build and test

The project uses CMake, a C++20 compiler, and (optionally) CUDA. A typical
configuration from a Visual Studio developer prompt is:

    cmake -G Ninja -S . -B build -DNUMAFABRIC_ENABLE_CUDA=ON
    cmake --build build
    ctest --test-dir build --output-on-failure

Project-controlled C++ source must compile cleanly under /W4 /WX (MSVC).
CUDA source must also be warning-clean for project-controlled code. If an
unavoidable warning originates solely from vendor/generated host stubs,
suppress it narrowly at the responsible target, never by weakening warnings
globally.

## Code review

All changes require review. The maintainer will review your pull request and
may request changes. Please be responsive and keep the change focused.

## Licensing

By contributing, you agree your contributions are licensed under the Apache
License, Version 2.0. The NOTICE file identifies the copyright holder.
