# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 0.x.x   | Development — best-effort |

## Reporting a Vulnerability

Please report security vulnerabilities via GitHub Issues with the `security` label,
or contact the maintainers directly.

Do NOT open public issues for active exploits.

## Security Practices

- All code runs through ASan, UBSan, MSan, and TSan
- Static analysis: cppcheck, PVS-Studio, CodeChecker (Clang SA + clang-tidy)
- Fuzz testing: LibFuzzer targets in `tests/fuzz/`
- Hardening flags: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, full RELRO
- Constant-time crypto comparisons via wolfCrypt
- Secret zeroing with `memset_explicit()` / `explicit_bzero()`
