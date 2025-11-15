

# --- Compiler and Flags ---
CC = gcc
# Common flags from your source files: -Wall -Wextra -O2
CFLAGS = -Wall -Wextra -O2

# --- Directories ---
SRC_DIR = src
BIN_DIR = bin

# --- External Dependencies ---
# These files were NOT provided, so we assume they exist in the project root.
# You may need to update these paths or add rules to build them.
CORTEZ_IPC_OBJ  = cortez_ipc.o
CORTEZ_MESH_OBJ = cortez-mesh.o
CTZ_JSON_LIB    = ctz-json.a

# --- Libraries ---
LIBS_PTHREAD = -pthread
LIBS_NCURSES = -lncursesw
LIBS_MATH_ZLIB = -lm -lz

# --- Source Files (for dependency tracking) ---
HDR_COMMON = $(SRC_DIR)/exodus-common.h

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

# --- Main Rules ---

# Default target: build all binaries
all: $(TARGETS)

# Rule to create the bin directory (order-only prerequisite)
$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# --- Individual Binary Build Rules ---
# We use specific rules because binary names don't always match .c file names
# and dependencies differ.

# 1. exctl
$(BIN_DIR)/exctl: $(SRC_DIR)/exctl.c $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exctl.c $(CTZ_JSON_LIB)

# 2. exodus
$(BIN_DIR)/exodus: $(SRC_DIR)/exodus.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CORTEZ_IPC_OBJ) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CORTEZ_IPC_OBJ) $(LIBS_PTHREAD)

# 3. exodus_snapshot (from exodus-anchor-weaver.c)
$(BIN_DIR)/exodus_snapshot: $(SRC_DIR)/exodus-anchor-weaver.c $(CORTEZ_IPC_OBJ) $(CTZ_JSON_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-anchor-weaver.c $(CORTEZ_IPC_OBJ) $(CTZ_JSON_LIB) $(LIBS_MATH_ZLIB)

# 4. cloud_daemon (from exodus-cloud-daemon.c)
$(BIN_DIR)/cloud_daemon: $(SRC_DIR)/exodus-cloud-daemon.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-cloud-daemon.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(LIBS_PTHREAD)

# 5. exodus-node-guardian
$(BIN_DIR)/exodus-node-guardian: $(SRC_DIR)/exodus-node-guardian.c $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-node-guardian.c $(CTZ_JSON_LIB) $(LIBS_PTHREAD)

# 6. query_daemon (from exodus-query-daemon.c)
$(BIN_DIR)/query_daemon: $(SRC_DIR)/exodus-query-daemon.c $(CORTEZ_MESH_OBJ) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-query-daemon.c $(CORTEZ_MESH_OBJ) $(LIBS_PTHREAD)

# 7. exodus-tui
$(BIN_DIR)/exodus-tui: $(SRC_DIR)/exodus-tui.c $(CTZ_JSON_LIB) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-tui.c $(CTZ_JSON_LIB) $(LIBS_NCURSES)

# 8. node-editor
$(BIN_DIR)/node-editor: $(SRC_DIR)/node-editor.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/node-editor.c $(LIBS_NCURSES)

$(BIN_DIR)/exodus-signal: $(SRC_DIR)/exodus-signal.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus-signal.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(LIBS_PTHREAD)

# --- Cleanup Rule ---
clean:
	@echo "Cleaning up $(BIN_DIR)..."
	@rm -rf $(BIN_DIR)

# --- Phony Targets ---
.PHONY: all clean
