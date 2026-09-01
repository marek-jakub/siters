"""Dogtail type stub -- configuration.

Dogtail ships without type information; this stub lets pyright/basedpyright
type-check its use in tests/test_siters_gui.py.

Only the fields the tests actually set are declared (debug_file,
debug_searching; see test setUp). Dogtail's real Config has many more
settings and allows arbitrary runtime attributes, but we deliberately do NOT
add a `__getattr__ -> Any` catch-all -- keep this stub explicit and extend it
only when a test needs another field.
"""
class Config:
    debug_file: str
    debug_searching: bool

config: Config
