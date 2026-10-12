# Wire smoke tests

The remaining `unit` label selects real-filesystem mail read/publication tests
in the top-level `tests/` directory, not broad domain matrices:

```bash
ctest --test-dir build -L unit --output-on-failure
```

See [`../ANATOMY.md`](../ANATOMY.md) for exact targets, injected roots and limits.
