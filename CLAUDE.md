# CLAUDE.md

[... existing content remains unchanged ...]

## Memories

- Use the system design plan from @docs/Crypto Market Data Provider_.md as the primary reference point for implementation
- Please use C++20 in your code. make sure it has clean formatting. For configuration files please use toml and toml format with toml++ module
- The codebase is targeting Linux hosts only - no Windows or macOS compatibility needed
- Focus on ultra-low latency and deterministic performance - every nanosecond matters
- Memory pool implementation has been thoroughly reviewed and hardened against production edge cases
- All code must be compiled with Clang 17+ for optimal HFT performance
- You need to use new style docker command, such as "docker compose" instead of "docker-compose"
- Always think hard. Always Ultrathink.
- Before proposing code changes, think hard to make sure you don't break current code functionalities, such as API protocols, dev environment assumptions, features, etc. 
- Think hard to not introducing new bugs when proposing a fix to the existing bugs.
- Run any code in the dev-container.
- Always think hard to present multiple options as you can. Don't rush to fix. Ultrathink and compare pros and cons of each option.
- Always use @scripts/build.sh to build. Remember always build in docker.
- **We should never use try-catch except in test cases, given this is a HFT low latency application.**