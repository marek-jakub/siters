BUILD_DIR ?= build
CMAKE ?= cmake
INSTALL_PREFIX ?= /usr/local
DATADIR ?= $(INSTALL_PREFIX)/share/siters

.PHONY: all build test test-unit clean install help

all: build

build:
	@mkdir -p $(BUILD_DIR)
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DDATADIR=$(DATADIR)
	$(CMAKE) --build $(BUILD_DIR) -j$$(nproc)

test-unit:
	@mkdir -p $(BUILD_DIR)
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DDATADIR=$(DATADIR)
	$(CMAKE) --build $(BUILD_DIR) -j$$(nproc)
	cd $(BUILD_DIR) && ctest --output-on-failure

test: test-unit

install: build
	$(CMAKE) --install $(BUILD_DIR) --prefix $(INSTALL_PREFIX)

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Siters build system (CMake wrapper)"
	@echo ""
	@echo "Usage:"
	@echo "  make              - Build the application (Release)"
	@echo "  make test         - Build and run unit tests"
	@echo "  make install      - Build and install to prefix"
	@echo "  make clean        - Remove build directory"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  INSTALL_PREFIX=$(INSTALL_PREFIX)"
	@echo "  DATADIR=$(DATADIR)"
	@echo "  CMAKE=$(CMAKE)"
