"""Dogtail type stub -- re-export marker.

This package directory (typings/dogtail/) contains hand-written type stubs so
pyright/basedpyright can type-check tests/test_siters_gui.py, which uses the
dogtail GUI-automation library. Dogtail provides no type information of its
own; the `<repo root>/typings/` `stubPath` causes these files to be loaded
automatically.

Note (2026): the test file also carries `# type: ignore[import-untyped]`
comments on its dogtail imports; those suppress the "package exists but is
not typed" complaint, while the stubs provide the actual types.
"""
