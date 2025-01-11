# todo: python version, venv?
CC = g++ -Wall -g -std=c++17
PY = python3

.PHONY: all clean test update cpp

all: test

clean:
	rm -rf a.out
	$(PY) -Bc "import pathlib; [p.unlink() for p in pathlib.Path('.').rglob('*.py[co]')]"  # delete .pyc and .pyo files
	$(PY) -Bc "import pathlib; [p.rmdir() for p in pathlib.Path('.').rglob('__pycache__')]"  # delete __pycache__ directories

test: cpp
	@# elaborate scheme to:
	@# - run c++ server in background,
	@# - run pytest,
	@# - wait for c++ server to terminate, and
	@# - verify both exited with 0
	@./a.out & $(PY) -m pytest -v; \
		P=$$?; \
		wait $$!; \
		(exit $$?) && (exit $$P) && true || false
	@echo "all tests passed"

update:
	git submodule foreach 'git pull origin main && git submodule update --init'

cpp: cpp/server.h cpp/test.cc
	@$(CC) cpp/test.cc
