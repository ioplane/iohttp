# Support

## Getting Help

- **Documentation**: See `docs/en/` for English docs, `docs/ru/` for Russian
- **Issues**: Use [GitHub Issues](../../issues) to report bugs or request features
- **Architecture**: See `docs/en/01-architecture.md`
- **Comparison**: See `docs/en/04-framework-comparison.md`

## Build Problems

Ensure you are building inside the dev container:

```bash
podman build -t iohttp-dev:latest -f deploy/podman/Containerfile .
podman run --rm -it --security-opt seccomp=unconfined \
  -v $(pwd):/workspace:Z localhost/iohttp-dev:latest
```
