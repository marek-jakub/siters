from siters import Siters
from PySide6 import QtCore

def test_siters_creation(qtbot):
    """Test that the Siters window can be created and displayed.
    
    Args:
        qtbot: pytest-qt fixture that provides QTest-like functionality
    """
    siters = Siters()
    qtbot.addWidget(siters)
    siters.show()
    assert siters.isVisible()
    assert siters.isEnabled()

def test_siters_window_title(qtbot):
    """Test that the Siters window has the correct title.
    
    Args:
        qtbot: pytest-qt fixture that provides QTest-like functionality
    """
    siters = Siters()
    qtbot.addWidget(siters)
    siters.show()
    assert siters.windowTitle() == "Siters"

def test_siters_window_size(qtbot):
    """Test that the Siters window has the correct size.
    
    Args:
        qtbot: pytest-qt fixture that provides QTest-like functionality
    """
    siters = Siters()
    qtbot.addWidget(siters)
    siters.show()
    assert siters.size() == QtCore.QSize(900, 900)

