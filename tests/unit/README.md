# Unit tests

Pure logic tests with no QWidget interaction live here once migrated from the
top-level `tests/` directory.

Today most unit targets still sit beside this folder (e.g.
`tests/kanban_model_test.cpp`). CTest labels them `unit`; filter with:

```bash
ctest -L unit --output-on-failure
```
