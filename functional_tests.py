from siters import Siters
from PySide6 import QtCore

def test_siter_creation(qtbot):
    siters = Siters()
    qtbot.addWidget(siters)
    siters.show()
    assert siters.isVisible()
    assert siters.isEnabled()

def test_siter_window_title(qtbot):
    siters = Siters()
    qtbot.addWidget(siters)
    siters.show()
    assert siters.windowTitle() == "Siters"