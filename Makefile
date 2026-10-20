# Cross-platform configuration: POSIX Linux vs Windows (MinGW)
ifeq ($(OS),Windows_NT)
    PTHREAD =
    LIBS = -lws2_32
    EXE = .exe
    RM = del /Q /F
    MKDIR = if not exist $(subst /,\,$(1)) mkdir $(subst /,\,$(1))
    FIXPATH = $(subst /,\,$1)
    PYTHON = python
else
    PTHREAD = -pthread
    LIBS = -pthread
    EXE =
    RM = rm -rf
    MKDIR = mkdir -p $(1)
    FIXPATH = $1
    PYTHON = python3
endif

CC = gcc
INCLUDES = -Iinclude
CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c11 $(PTHREAD) -O2

BIN_DIR = bin
OBJ_DIR = obj

# Object Files
CORE_OBJS = $(OBJ_DIR)/rudp_platform.o \
            $(OBJ_DIR)/rudp_packet.o \
            $(OBJ_DIR)/rudp_window.o \
            $(OBJ_DIR)/rudp_socket.o \
            $(OBJ_DIR)/rudp_crypto.o

SIM_OBJS  = $(OBJ_DIR)/loss_proxy.o \
            $(OBJ_DIR)/rudp_platform.o \
            $(OBJ_DIR)/rudp_packet.o

RECV_OBJS = $(OBJ_DIR)/rudp_receiver.o $(CORE_OBJS)
SEND_OBJS = $(OBJ_DIR)/rudp_sender.o $(CORE_OBJS)
TEST_OBJS = $(OBJ_DIR)/test_unit.o $(CORE_OBJS)

# Binaries
SIM_BIN   = $(BIN_DIR)/loss_proxy$(EXE)
RECV_BIN  = $(BIN_DIR)/rudp_receiver$(EXE)
SEND_BIN  = $(BIN_DIR)/rudp_sender$(EXE)
TEST_BIN  = $(BIN_DIR)/test_unit$(EXE)

.PHONY: all clean sim demo tests test

all: sim demo tests

# Directories
$(OBJ_DIR):
	@$(call MKDIR, $(OBJ_DIR))

$(BIN_DIR):
	@$(call MKDIR, $(BIN_DIR))

# Core object rules
$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Simulator rule
$(OBJ_DIR)/loss_proxy.o: sim/loss_proxy.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Demo rules
$(OBJ_DIR)/rudp_receiver.o: demo/rudp_receiver.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/rudp_sender.o: demo/rudp_sender.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Test rules
$(OBJ_DIR)/test_unit.o: tests/test_unit.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Target: sim
sim: $(SIM_BIN)

$(SIM_BIN): $(SIM_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Target: demo
demo: $(RECV_BIN) $(SEND_BIN)

$(RECV_BIN): $(RECV_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

$(SEND_BIN): $(SEND_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Target: tests
tests: $(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Target: test (Unit Tests + End-to-End Impairment Test)
test: all
	@echo =======================================================
	@echo Running Unit Test Suite...
	@echo =======================================================
	$(TEST_BIN)
	@echo =======================================================
	@echo Running End-to-End File Transfer over Loss Proxy (15% Loss)...
	@echo =======================================================
	$(PYTHON) tests/run_e2e_test.py

clean:
	@echo Cleaning build artifacts...
ifeq ($(OS),Windows_NT)
	-$(RM) $(call FIXPATH, $(OBJ_DIR)/*.o) 2>nul
	-$(RM) $(call FIXPATH, $(BIN_DIR)/*$(EXE)) 2>nul
	-$(RM) test_input* 2>nul
	-$(RM) received* 2>nul
	-$(RM) *.log 2>nul
else
	$(RM) $(OBJ_DIR)/*.o $(BIN_DIR)/*$(EXE) test_input* received* *.log
endif
