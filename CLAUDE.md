# CLAUDE.md

## Memories

- Use the comprehensive architectural guide from @docs/arch_guide.md as the primary reference point for implementation
  - This guide provides detailed specifications for components, thread model and design principles.
- Make sure all code in C++ is compatible with C++20 standard.
- For configuration files, use toml format with toml++ module.
- The codebase is targeting Linux hosts only - no Windows or macOS compatibility needed.
- Code on the hot-path should focus on ultra-low latency and deterministic performance - every nanosecond matters.
- You need to use new style docker command, such as "docker compose" instead of "docker-compose"
- Before proposing code changes, think hard to make sure you don't break current code functionalities, such as features and API contract . 
- Think hard to not introducing new bugs when proposing a fix to the existing bugs.
- When planning, always think hard to present multiple options as you can. Don't rush to fix. Ultrathink and compare pros and cons of each option.
- Always run code and build in the dev docker container `crypto-lob-dev`, and use @scripts/build.sh to build.
- We should never use try-catch except in test cases, given this is a HFT low latency application. Use std::terminate to fail fast.