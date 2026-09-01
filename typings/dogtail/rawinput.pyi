"""Dogtail type stub -- raw input helpers.

Dogtail ships without type information; this stub lets pyright/basedpyright
type-check its use in tests/test_siters_gui.py. (Keep signatures explicit and
extend them only when a test needs a new capability.)
"""
def pressKey(keyName: str) -> None: ...

def keyCombo(combo: str) -> None: ...

def click(x: int, y: int, button: int = 1) -> None: ...
