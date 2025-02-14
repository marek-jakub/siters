from PySide6.QtWidgets import (QMainWindow)

class Siters(QMainWindow):
    """Main window class for the Siters PDF viewer application.
    """

    def __init__(self):
        super().__init__()

        self.initializeUI()

    def initializeUI(self):
        self.setMinimumSize(900,900)
        self.setWindowTitle("Siters")
