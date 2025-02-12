#!/usr/bin/env python3

# License Notice for Qt 6.8
# This application uses Qt version 6.8, which is licensed under the (L)GPL v3.
# You can find the source code for Qt 6.8 at https://www.qt.io/download
# The LGPL v3 license can be found at https://www.gnu.org/licenses/lgpl-3.0.html

import sys
from siters import Siters
from PySide6.QtWidgets import QApplication


if __name__ == "__main__":
    app = QApplication(sys.argv)
    siters = Siters()
    siters.show()
    app.exec()
