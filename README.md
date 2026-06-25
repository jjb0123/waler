# waler

Early-stage C++ repo for packet-generation and trading-system experiments.

## Layout

- `src/waler/`: reusable library code in the `waler` namespace
- `src/pktgen/`: small executable for exercising packet generation
- `benchmarks/`: standalone benchmark code and outputs
- `common/`: older shared helpers from pre-CMake experiments

## Build

```sh
make
```

Built executables are placed in `build/bin/`.
