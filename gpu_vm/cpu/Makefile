# gvm_cpu and its tests (gpu_vm/cpu/README.md): make; make test. Output in build/gvm_cpu (the repository's build/).
CC ?= cc
CXX ?= c++
OUT ?= ../../build/gvm_cpu
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter

all: $(OUT)/gvm_cpu

$(OUT)/gvm_cpu: gvm_cpu.c ../ref/gvm_ref.h ../isa.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ gvm_cpu.c -lm

$(OUT)/mk: tests/mk.cpp ../ref/gvm_asm.h ../isa.h
	mkdir -p $(OUT)
	$(CXX) -O1 -std=c++17 -o $@ tests/mk.cpp

test: $(OUT)/gvm_cpu $(OUT)/mk
	$(OUT)/mk $(OUT)
	sh tests/run.sh $(OUT)/gvm_cpu $(OUT)

.PHONY: all test
