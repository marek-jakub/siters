"""Dogtail type stub -- launch helper.

Dogtail ships without type information; this stub lets pyright/basedpyright
type-check its use in tests/test_siters_gui.py.

`run(...)` launches the app under test. It is declared with an explicit,
narrowed signature (source, timeout, dumb) matching how the tests call it:
`run(self.siters_binary, timeout=5, dumb=True)`. If a test needs a different
argument, extend this signature -- do not fall back to `*args: Any`.
"""
from dogtail.tree import Node

def run(source: str, timeout: float = ..., dumb: bool = ...) -> Node: ...
