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

    # The title should be "Siters"
    assert siters.windowTitle() == "Siters"

    # The initial window size should be 900x900 pixels
    assert siters.size() == QtCore.QSize(900, 900)
