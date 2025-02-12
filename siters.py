from PySide6.QtWidgets import (QMainWindow)

class Siters(QMainWindow):

    def __init__(self):
        super().__init__()

        self.initializeUI()

    def initializeUI(self):
        self.setMinimumSize(900,900)
        self.setWindowTitle("Siters")
