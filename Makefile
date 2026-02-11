# --- Compiler and Flags ---
CC = gcc

# Common flags from your source files: -Wall -Wextra -O2

CFLAGS = -Wall -Wextra -O2 -static
CFL = -Wall -O2

#Directories

SRC_DIR = src
BIN_DIR = bin
SRV = server-side
SHR = shared

#Headers

INC = -Iinclude -Ik-module
INCL = include

#External Dependencies

CORTEZ_IPC_OBJ  = $(SHR)/cortez_ipc.o
CORTEZ_MESH_OBJ = $(SHR)/cortez-mesh.o
CTZ_JSON_LIB    = $(SHR)/ctz-json.o
CTZ_SET = $(SHR)/ctz-set.o

# --- Libraries ---
LIBS_PTHREAD = -pthread
LIBS_NCURSES = -lncursesw -ltinfo
LIBS_MATH_ZLIB = -lm -lz

# --- Source Files (for dependency tracking) ---
HDR_COMMON = $(INCL)/exodus-common.h

# --- Target Binaries ---
TARGETS = \
	$(BIN_DIR)/exctl \
	$(BIN_DIR)/exodus \
	$(BIN_DIR)/exodus_snapshot \
	$(BIN_DIR)/cloud_daemon \
	$(BIN_DIR)/exodus-node-guardian \
	$(BIN_DIR)/query_daemon \
	$(BIN_DIR)/exodus-tui \
	$(BIN_DIR)/node-editor \
	$(BIN_DIR)/exodus-signal

LIBRARIES = \
	$(SRC_DIR)/ctz-json.c \
	$(SRC_DIR)/ctz-set.c \
	$(SRC_DIR)/cortez-mesh \
	$(SRC_DIR)/cortez-ipc.c

SERVER = $(SRV)/exodus-coordinator.c

SRV_OUT = s-bin

STARGET = $(SRV_OUT)/exodus-coordinator

# --- Main Rules ---

# Default target: build all binaries
all: $(TARGETS)

# Rule to create the bin directory (order-only prerequisite)
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# --- Individual Binary Build Rules ---

# 1. exctl
$(BIN_DIR)/exctl: $(SRC_DIR)/exctl.c $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exctl.c $(CTZ_JSON_LIB) $(INC)

# 2. exodus
$(BIN_DIR)/exodus: $(SRC_DIR)/exodus.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CORTEZ_IPC_OBJ) $(CTZ_SET) $(SHR)/autosuggest.o $(SHR)/auto-nav.o $(SHR)/errors.o $(SHR)/signals.o $(SHR)/interrupts.o $(SHR)/child_handler.o $(SHR)/utils.o $(SHR)/kernel_repl.o $(SHR)/syscall_commands.o $(SHR)/excon_io.o $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CORTEZ_IPC_OBJ) $(CTZ_SET) $(SHR)/autosuggest.o $(SHR)/auto-nav.o $(SHR)/errors.o $(SHR)/signals.o $(SHR)/interrupts.o $(SHR)/child_handler.o $(SHR)/utils.o $(SHR)/kernel_repl.o $(SHR)/syscall_commands.o $(SHR)/excon_io.o $(LIBS_PTHREAD) $(INC)

$(SHR)/utils.o: $(SRC_DIR)/utils.c $(INCL)/utils.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/utils.c -o $@ $(INC)

$(SHR)/autosuggest.o: $(SRC_DIR)/autosuggest.c $(INCL)/autosuggest.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/autosuggest.c -o $@ $(INC)

$(SHR)/auto-nav.o: $(SRC_DIR)/auto-nav.c $(INCL)/auto-nav.h $(INCL)/autosuggest.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/auto-nav.c -o $@ $(INC)

$(SHR)/errors.o: $(SRC_DIR)/errors.c $(INCL)/errors.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/errors.c -o $@ $(INC)

$(SHR)/signals.o: $(SRC_DIR)/signals.c $(INCL)/signals.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/signals.c -o $@ $(INC)

$(SHR)/interrupts.o: $(SRC_DIR)/interrupts.c $(INCL)/interrupts.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/interrupts.c -o $@ $(INC)


$(SHR)/child_handler.o: $(SRC_DIR)/child_handler.c $(INCL)/child_handler.h $(INCL)/interrupts.h $(INCL)/signals.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/child_handler.c -o $@ $(INC)

$(SHR)/kernel_repl.o: $(SRC_DIR)/kernel_repl.c $(INCL)/kernel_repl.h $(INCL)/syscall_commands.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/kernel_repl.c -o $@ $(INC)

$(SHR)/syscall_commands.o: $(SRC_DIR)/syscall_commands.c $(INCL)/syscall_commands.h $(INCL)/kernel_repl.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/syscall_commands.c -o $@ $(INC)

$(SHR)/excon_io.o: $(SRC_DIR)/excon_io.c $(INCL)/excon_io.h | $(SHR)
	$(CC) $(CFL) -c $(SRC_DIR)/excon_io.c -o $@ $(INC)

# 3. exodus_snapshot (from exodus-anchor-weaver.c)
$(BIN_DIR)/exodus_snapshot: $(SRC_DIR)/exodus-anchor-weaver.c $(CORTEZ_IPC_OBJ) $(CTZ_JSON_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-anchor-weaver.c $(CORTEZ_IPC_OBJ) $(CTZ_JSON_LIB) $(LIBS_MATH_ZLIB) $(INC)

# 4. cloud_daemon (from exodus-cloud-daemon.c)
$(BIN_DIR)/cloud_daemon: $(SRC_DIR)/exodus-cloud-daemon.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-cloud-daemon.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(LIBS_PTHREAD) $(INC)

# 5. exodus-node-guardian
$(BIN_DIR)/exodus-node-guardian: $(SRC_DIR)/exodus-node-guardian.c $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-node-guardian.c $(CTZ_JSON_LIB) $(LIBS_PTHREAD) $(INC)

# 6. query_daemon (from exodus-query-daemon.c)
$(BIN_DIR)/query_daemon: $(SRC_DIR)/exodus-query-daemon.c $(CORTEZ_MESH_OBJ) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-query-daemon.c $(CORTEZ_MESH_OBJ) $(LIBS_PTHREAD) $(INC)

# 7. exodus-tui
$(BIN_DIR)/exodus-tui: $(SRC_DIR)/exodus-tui.c $(CTZ_JSON_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-tui.c $(CTZ_JSON_LIB) $(LIBS_NCURSES) $(INC)

# 8. node-editor
$(BIN_DIR)/node-editor: $(SRC_DIR)/node-editor.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/node-editor.c $(LIBS_NCURSES) $(INC)

$(BIN_DIR)/exodus-signal: $(SRC_DIR)/exodus-signal.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CTZ_SET) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-signal.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CTZ_SET) $(LIBS_PTHREAD) $(INC)

#Compile Server
server: $(SRV_OUT)
	@echo "Compiling $(SERVER)"
	$(CC) $(CFLAGS) $(SERVER) $(CTZ_JSON_LIB) -o $(STARGET) $(LIBS_PTHREAD) $(INC)

#Compile Libraries
#Compile Libraries
lib: $(SHR)/ctz-set.o $(SHR)/ctz-json.o $(SHR)/cortez-mesh.o $(SHR)/cortez_ipc.o

$(SHR)/ctz-set.o: $(SRC_DIR)/ctz-set.c
	$(CC) -c $< -o $@ $(CFL) $(INC)

$(SHR)/ctz-json.o: $(SRC_DIR)/ctz-json.c
	$(CC) -c $< -o $@ $(CFL) $(INC)

$(SHR)/cortez-mesh.o: $(SRC_DIR)/cortez-mesh.c
	$(CC) -c $< -o $@ $(CFL) $(INC)

$(SHR)/cortez_ipc.o: $(SRC_DIR)/cortez_ipc.c
	$(CC) -c $< -o $@ $(CFL) $(INC)


$(SRV_OUT):
	@echo "Creating $(SRV_OUT)"
	@mkdir -p $(SRV_OUT)


# --- Cleanup Rule ---
# --- Cleanup Rule ---
clean:
	@echo "Cleaning up $(BIN_DIR)..."
	@rm -f $(BIN_DIR)/exodus \
	       $(BIN_DIR)/exodus-tui \
	       $(BIN_DIR)/node-editor \
	       $(BIN_DIR)/cloud_daemon \
	       $(BIN_DIR)/query_daemon \
	       $(BIN_DIR)/exodus-node-guardian \
	       $(BIN_DIR)/exctl \
	       $(BIN_DIR)/exodus-signal \
	       $(BIN_DIR)/exodus_snapshot
	@echo "Cleaning up $(SRV_OUT)"
	@rm -f $(SRV_OUT)/exodus-coordinator
	@rm -f $(SHR)/*.o

# --- Phony Targets ---
.PHONY: all clean
