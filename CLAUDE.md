# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a C++ cryptocurrency market data provider system designed for high-frequency trading (HFT) applications. The system focuses on ultra-low latency, high throughput, and predictable performance for processing real-time market data from multiple cryptocurrency exchanges.

[... existing content remains unchanged ...]

## Memories

- Use the system design plan from @Crypto Market Data Provider_.md as the primary reference point for implementation
- Please use C++20 in your code. make sure it has clean formatting. For configuration files please use toml and toml format with toml++ module
- The codebase is targeting Linux hosts only - no Windows or macOS compatibility needed
- Focus on ultra-low latency and deterministic performance - every nanosecond matters
- All code must be compiled with Clang 17+ for optimal HFT performance
- You need to use new style docker command, such as "docker compose" instead of "docker-compose"
- Always think hard. Always ultrathink. Always think twice. Before proposing code changes, think hard to make sure you don't break current code functionalities, such as API protocols, dev environment assumptions, features, etc. Think hard to not introducing new bugs when proposing a fix to the existing bugs. Always take a hard look of the larger window context of the code you are changing.
- Remember all dev work need to be done in a linux devcontainer, not on this MacBook host
- Run any code in devcontainer.
- Always think hard to present multiple options as you can. Don't rush to fix. Ultrathink and compare pros and cons of each option based on principles in @CLAUDE.md and @Crypto Market Data Provider_.md