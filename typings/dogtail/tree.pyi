"""Dogtail type stub.

Dogtail (the AT-SPI GUI-automation library used by tests/test_siters_gui.py)
ships without type information, so pyright/basedpyright would otherwise report
"Stub file not found for dogtail" and treat all dogtail calls as untyped.

This directory (typings/dogtail/) is the default stub location for
pyright/basedpyright (the `<repo root>/typings/` `stubPath`), so these files
are picked up automatically with no configuration.

These stubs deliberately declare ONLY the dogtail APIs the GUI tests actually
use, and model them as explicitly as possible (no `Any`/catch-all). If a test
starts using a dogtail member not declared here, the type checkers will report
a hard error -- add that member to this stub explicitly (see `Config`/`run`
for the pattern) rather than reintroducing `__getattr__ -> Any`.
"""
from collections.abc import Callable
from typing import TypeAlias

Predicate: TypeAlias = Callable[[Node], object]


class SearchError(Exception): ...


class Node:
    name: str
    roleName: str
    text: str | None
    position: tuple[int, int]
    children: list[Node]

    def findChild(
        self,
        predicate: Predicate,
        recursive: bool = True,
        showingOnly: bool = True,
        isLambda: bool = False,
    ) -> Node: ...

    def findChildren(
        self,
        predicate: Predicate,
        recursive: bool = True,
        showingOnly: bool = True,
        isLambda: bool = False,
    ) -> list[Node]: ...

    def click(self, button: int = 1) -> None: ...

    def do_action(self, index: int = 0) -> None: ...

    def keyCombo(self, combo: str) -> None: ...

    def typeText(self, text: str) -> None: ...

    def grab_focus(self) -> None: ...

    def kill(self) -> None: ...

    def application(self, name: str) -> Node: ...


root: Node

