# Siters Test Suite

This directory contains unit tests and GUI tests for the Siters PDF viewer application.

## Test Types

### 1. Unit Tests (CMocka)

Located in: `test_siters_unit.c`

**Description:** Unit tests for core functionality using CMocka mocking framework.

**Tests included:**
- `test_create_main_window_returns_window` - Verifies window creation returns valid widget
- `test_create_main_window_sets_title` - Checks window title is set correctly
- `test_create_main_window_sets_size` - Validates default window size
- `test_create_main_window_has_destroy_signal` - Confirms destroy signal is connected
- `test_create_multiple_windows` - Tests multiple independent window creation
- `test_create_main_window_type` - Verifies window widget type

**Building unit tests:**
```bash
make -f Makefile.tests build-unit-tests
```

**Running unit tests:**
```bash
make -f Makefile.tests run-unit-tests
```

**Requirements:**
- GCC or Clang compiler
- CMocka library (`libcmocka-dev` on Ubuntu/Debian)
- GTK+3 development files (`libgtk-3-dev`)
- Poppler-glib development files (`libpoppler-glib-dev`)

**Install dependencies (Ubuntu/Debian):**
```bash
sudo apt install libcmocka-dev libgtk-3-dev libpoppler-glib-dev
```

### 2. GUI Tests (Dogtail)

Located in: `test_siters_gui.py`

**Description:** GUI tests for the Siters application using Dogtail and AT-SPI accessibility framework.

**Test classes:**

- `TestSitersWindowProperties` - Tests window properties
  - `test_window_exists` - Verifies main window exists
  - `test_window_title` - Checks window title
  - `test_window_is_visible` - Validates window visibility

- `TestSitersApplicationBehavior` - Tests application behavior
  - `test_application_launches` - Verifies successful launch
  - `test_application_responsive` - Checks responsiveness
  - `test_window_can_be_closed` - Tests graceful shutdown

- `TestSitersWindowControls` - Tests window controls and UI elements
  - `test_find_window_frame` - Attempts to find window frame
  - `test_application_focus` - Verifies focus capability

- `TestSitersAccessibility` - Tests AT-SPI accessibility
  - `test_accessible_application_name` - Checks accessible name
  - `test_accessible_role` - Validates accessible role

**Running GUI tests:**
```bash
make -f Makefile.tests run-gui-tests
```

**Requirements:**
- Python 3
- Dogtail package
- PyAtSpi and PyGObject (AT-SPI Python bindings)
- X11 display server
- AT-SPI accessibility daemon running

**Install dependencies:**
```bash
pip install dogtail python3-pyatspi PyGObject
```

**Important Notes:**
- GUI tests require the AT-SPI accessibility daemon to be running
- If accessibility is not available, tests will gracefully skip with a message
- For interactive testing on Linux desktop, ensure accessibility is enabled:
  ```bash
  systemctl --user start at-spi-dbus-bus.service
  ```

**Run with Xvfb (headless):**
```bash
sudo apt install xvfb
xvfb-run -a python3 tests/test_siters_gui.py
```

**When to expect test results:**
- **Tests run**: When X11 display and AT-SPI daemon are available
- **Tests skip**: When accessibility is not available (non-GUI environment, headless server)
- Both outcomes are acceptable and expected depending on environment

## Using CMake

Instead of Makefile, you can use CMake:

```bash
# Configure
cmake -B build

# Build
cmake --build build

# Run tests
ctest --test-dir build --verbose
```

**Using CMake:**
```bash
cmake -B build && cmake --build build && ctest --test-dir build --verbose
```

## Running All Tests

cmake commands need to be run in order to access build files in the ./build directory.

**Using Makefile:**
```bash
make -f Makefile.tests test          # Run unit tests only
make -f Makefile.tests run-gui-tests # Run GUI tests
make -f Makefile.tests all           # Display help
```

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Tests
on: [push, pull_request]

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: sudo apt install libcmocka-dev libgtk-3-dev libpoppler-glib-dev
      - name: Build and test
        run: make -f Makefile.tests run-unit-tests

  gui-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: |
          sudo apt install libgtk-3-dev libpoppler-glib-dev
          pip install dogtail python3-pyatspi
      - name: Build and test
        run: make -f Makefile.tests run-gui-tests
```

## Test Output

### Unit Tests Output Example:
```
[==========] Running 6 test(s).
[       OK ] test_create_main_window_returns_window
[       OK ] test_create_main_window_sets_title
[       OK ] test_create_main_window_sets_size
[       OK ] test_create_main_window_has_destroy_signal
[       OK ] test_create_multiple_windows
[       OK ] test_create_main_window_type
[==========] 6 test(s) run.
```

### GUI Tests Output Example:
```
test_application_is_process (__main__.TestSitersBasicOperation.test_application_is_process)
Test that Siters process is running. ... ok
test_application_starts (__main__.TestSitersBasicOperation.test_application_starts)
Test that Siters application starts and can be accessed via AT-SPI. ... SUCCESS: Application is accessible via AT-SPI
ok
...
```

## Troubleshooting

### CMocka tests won't compile
- Ensure `libcmocka-dev` is installed
- Check that pkg-config can find cmocka: `pkg-config --cflags cmocka`

### GUI tests can't find the application
- Ensure the application is built: `make -f Makefile.tests build-app`
- Check that the app is in PATH or use absolute path
- Verify AT-SPI is running: `ps aux | grep at-spi`

### AT-SPI not available
- Install AT-SPI daemon: `sudo apt install at-spi2-core`
- Start service: `systemctl --user start at-spi-dbus-bus.service`

### Tests timeout or fail
- Check system resources (CPU, memory)
- Increase timeouts in test files if necessary
- Run single test class: `python3 -m unittest test_siters_gui.TestSitersWindowProperties`

## Adding New Tests

### Adding Unit Tests (CMocka)

1. Add new test function to `test_siters_unit.c`:
```c
static void test_new_feature(void **state) {
    (void) state;
    
    // Setup
    // Test code
    
    assert_int_equal(expected, actual);
}
```

2. Register test in `main()`:
```c
cmocka_unit_test(test_new_feature),
```

### Adding GUI Tests (Dogtail)

1. Add new test class to `test_siters_gui.py`:
```python
class TestNewFeature(SitersGUITestCase):
    def test_new_behavior(self):
        try:
            siters_app = root.application("siters")
            # Test code
            self.assertTrue(condition)
        except Exception as e:
            self.fail(f"Test failed: {e}")
```

2. Add to test suite in `suite()` function

## Resources

- [CMocka Documentation](https://cmocka.org/)
- [Dogtail Documentation](https://accessibility.dev/dogtail/)
- [AT-SPI Specification](https://wiki.gnome.org/Accessibility)
- [GTK+ Documentation](https://developer.gnome.org/gtk3/)
