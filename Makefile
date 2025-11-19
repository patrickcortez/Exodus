

# --- Compiler and Flags ---
CC = gcc

# Common flags from your source files: -Wall -Wextra -O2

CFLAGS = -Wall -Wextra -O2
CFL = -Wall -O2

#Directories

SRC_DIR = src
BIN_DIR = bin
SRV = server-side
SHR = shared

#Headers

INC = -Iinclude
INCL = include

#External Dependencies

CORTEZ_IPC_OBJ  = $(SHR)/cortez_ipc.o
CORTEZ_MESH_OBJ = $(SHR)/cortez-mesh.o
CTZ_JSON_LIB    = $(SHR)/ctz-json.a
CTZ_SET = $(SHR)/ctz-set.a

# --- Libraries ---
LIBS_PTHREAD = -pthread
LIBS_NCURSES = -lncursesw
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
$(BIN_DIR)/exodus: $(SRC_DIR)/exodus.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CORTEZ_IPC_OBJ) $(CTZ_SET) $(HDR_COMMON) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC_DIR)/exodus.c $(CORTEZ_MESH_OBJ) $(CTZ_JSON_LIB) $(CORTEZ_IPC_OBJ) $(CTZ_SET) $(LIBS_PTHREAD) $(INC)

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
lib: $(LIBRARIES)

	$(CC) -c $(SRC_DIR)/ctz-set.c -o $(SHR)/ctz-set.o $(CFL) $(INC)

	$(CC) -c $(SRC_DIR)/ctz-json.c -o $(SHR)/ctz-json.o $(CFL) $(INC)

	$(CC) -c $(SRC_DIR)/cortez-mesh.c -o $(SHR)/cortez-mesh.o $(CFL) $(INC)

	$(CC) -c $(SRC_DIR)/cortez-ipc.c -o $(SHR)/cortez_ipc.o $(CFL) $(INC)


$(SRV_OUT):
	@echo "Creating $(SRV_OUT)"
	@mkdir -p $(SRV_OUT)


# --- Cleanup Rule ---
clean:
	@echo "Cleaning up $(BIN_DIR)..."
	@rm -rf $(BIN_DIR)
	@echo "Cleaning up $(SRV_OUT)"
	@rm -rf $(SRV_OUT)

# --- Phony Targets ---
.PHONY: all clean
