// SPDX-License-Identifier: MIT
//
// Benchmark driver. Measurements produced here are the only numbers allowed to
// appear in docs/benchmark-results.md — see docs/benchmarking.md for the rules.
#include <cstdio>

#include <vectordb/core/version.hpp>

int main() {
    std::fputs(vectordb::build_info().c_str(), stdout);
    std::fputs("\nNo benchmarks registered yet.\n", stdout);
    return 0;
}
