# Exodus

**Exodus** is a Real Time Multi Daemon File System Monitoring that uses Linux's **Inotify** & Version Control (C/Linux) that
handles large files, all made in C. It has its own TUI for Editing Text Files,IPC and Daemon Management tool.

---

## Installation

To set up exodus, you must first, make sure the installers have the proper permissions:

### 1. Make Installer and Uninstaller scripts executable
``` bash

chmod +x install

chmod +x install-k

chmod +x install-dependencies

chmod +x uninstall

```

### 2. Install Dependencies

``` bash

sudo ./install-dependencies.sh

```

### 3. Compile Kernel Module

``` bash

#This Kernel Module is important for the daemons to communicate in the mesh ipc.

cd k-module

make

cd ..

```

### 4. Compiling Binaries

``` bash

make

sudo ./install

sudo ./install-k

```

### 5. Quick Start

``` bash

# To start exodus

exodus start

# Add a Directory as a node

exodus add-node /home/user/nodename node-name

# View events of the node, You just added

exodus history node-name

# commit the changes of a node

exodus commit node-name 1.0

```

---

## Commands

[Daemon & Service Management]

  - start:        Start the Exodus cloud and query daemons
  - stop:         Stop the Exodus daemons

[Node Configuration & TUI]

  - node-conf:    Configure a node's auto-surveillance and settings
  - node-status:  Show uncommitted changes for a node
  - node-edit:    Open the TUI to browse and edit files in nodes with built in Text Editor
  - node-man:     Create, delete, move, or copy files/dirs within a node

[Snapshot & History Management]

  - commit:       Create a permanent, versioned snapshot of a node
  - rebuild:      Restore a node to a specific snapshot version (destructive)
  - checkout:     Restore a single file from a specific snapshot
  - diff:         Show changes between two snapshot versions
  - history:      View History of a node(what changed in a node e.g: Modified, Created, Moved or Deleted)
  - log:          Show the commit history for the active subsection
  - clean:        Clear the uncommitted change history for a node

[Subsection Management]
  - list-subs:    List all subsections for a node
  - add-subs:     Create a new subsection
  - remove-subs:  Remove a subsection (cannot remove 'master' or active subsection)
  - switch:       Switch active subsection (rebuilds node to new subsection's HEAD)
  - promote:      Promote (merge) a subsection into 'master' (Trunk)

[Archiving & Data Transfer]
  - pack:         Encrypt and archive a node into a .enode file
  - unpack:       Decrypt and extract a .enode file
  - pack-info:    Show metadata from an encrypted .enode file header
  - send:         Send a .enode file to a remote receiver
  - expose-node:  Start a receiver to accept .enode files

[Node Management]

  - add-node:     Adds your project/directory as a new node
  - list-nodes:   List all added nodes
  - remove-node:  Deletes a node and remove it from the config
  - view-node:    View recent events of a node
  - activate:     Start real-time surveillance on an inactive node
  - deactivate:   Stop real-time surveillance on an active node
  - attr-node:    Set metadata (author, tag, desc) for a node
  - info-node:    View metadata for a node
  - search-attr:  Find nodes by author or tag
  - look:         Find a file/folder, or pin it with 'look <file> --pin <name>'
  - unpin:        Remove a pinned shortcut

[File Indexing]

  - upload:       Upload a file for word indexing
  - find:         Find a word in the last indexed file
  - change:       Find and replace a word in the last indexed file   | fn ls path
  - wc:           Get the word count of the last indexed file
  - wl:           Get the line count of the last indexed file
  - cc:           Get the non-space character count of the last indexed file

[Unit and Network Utility/Management]
  - unit-list:    List all connected Units on the network
  - view-unit :    List all nodes on a specific remote Unit
  - sync:         Sync history with a remote node (e.g., sync <unit> <remote-node> <local-node>)
  - unit-set:     Set this machine's name or coordinator (--name, --coord)
  - view-cache    For debugging, views the local node list of the signal daemon, to make sure it's upto date.
  - push          Push a node to another unit in your network.
  - clone         Clone a Unit's node into your own unit(device).

---

## Concepts and Definitions

- Node: A directory on your filesystem that Exodus is tracking (e.g., /home/user/projects/my-site). This is the root of a versioned project.

- Unit: A Device/Machine that has Exodus, You can View, push or clone any Units Nodes.

- Coordinator: An Exodus Server for Synchronizing and Managing Units Across a Network.

- Daemon: A background process that runs the Exodus system.

- Cloud_daemon: The central orchestration daemon. It manages all nodes, handles versioning, and performs file system monitoring.

- Query_daemon: The public-facing daemon that accepts commands from the exodus client and forwards them to the cloud_daemon.

- Guardian (exodus-node-guardian): A special, lightweight, standalone daemon dedicated to a single node. It's used when a node is set to --auto 1, allowing it to be monitored on system startup without the main cloud_daemon running.

- exctl: A command-line tool (like systemctl) used to manage the standalone exodus-node-guardian daemons (exctl start <node>, exctl status <node>).

- Trunk: The default, primary line of history for a node. It is internally referred to as the "master" subsection.

- Subsection: A parallel line of development, equivalent to a "branch" in Git. You can create a subsection, make commits to it, and then merge it back into the Trunk.

- Commit: The action (exodus commit) of creating a permanent, named snapshot of a node's current state in the active subsection.

- T-Commit (Trunk Commit): A commit that is made directly to the master (Trunk) subsection.

- S-Commit (Subsection Commit): A commit that is made to any subsection other than master.

- Anchor: The specific T-Commit (Trunk commit) from which a subsection was originally created. This is used as the base for merging.

- Promote: The action (exodus promote) of merging a subsection's changes back into the master (Trunk) subsection. This performs a 3-way merge and creates a new T-Commit.

- SBDS (Subsection Binary Deconstruction System): The internal name for the large file handling system. It uses Content-Defined Chunking (CDC) to break large files into smaller, hashed blocks (.bblk files), which are tracked by a .mobj (manifest) file.

- Pack (.enode file): An encrypted, single-file archive of a node created with exodus pack. It is secured with AES-256 using your password and can be safely transferred.

- TUI (exodus-tui): The Terminal User Interface. It's the ncurses-based file explorer for browsing nodes, viewing file statuses, and opening the built-in editor.

- enode File: A Encrypted File that can be sent to other exodus users.

---

## Atomic — Kernel REPL

**Atomic** is a built-in kernel-facing REPL that provides direct access to Linux syscalls through a scripting environment. It runs inside the Exodus shell and renders through the kernel console subsystem (`exodus-term`).

### Entering the REPL

```bash
# From the exodus shell
atomic
```

You'll see the `atomic>` prompt. Type lines of code, then type `[EXECUTE]` on a new line to run the block. Press `Ctrl+C` to cancel.

### Running Scripts (.atom)

You can write Atomic scripts in a file with the `.atom` extension and execute them directly:

```bash
exodus script.atom
```

**Important**: Scripts must end with the `[EXECUTE]` command to trigger execution. If omitted, the script will load but not run.

### Syntax

#### Variables & Literals

```bash
var x = 42
var addr = 0xDEADBEEF
var mask = 0b1101
var msg = "hello world"
var active = true
var empty = null
```

Supported literal types:
| Type | Example | Description |
|------|---------|-------------|
| Decimal | `42`, `-1` | Signed integers |
| Hex | `0xFF`, `0xDEAD` | Memory addresses, flags, masks |
| Binary | `0b1010` | Bit patterns |
| String | `"hello"` | Text data |
| Boolean | `true`, `false` | Maps to `1` / `0` |

Reference variables with `$name`:
```bash
var fd = sys-open "/tmp/test" O_RDONLY
sys-write 1 $fd
```

#### Command Substitution
Capture the output of a command or function return value using `$(...)`:

```bash
var fd = $(sys-open "/tmp/test" O_RDONLY)
var sum = $(add 10 20)
sys-write 1 $(sys-uname)
```

#### Command Substitution vs. Direct Assignment

##### Direct Assignment
Direct assignment `var x = sys-open ...` works because `sys-` commands return values directly. It is slightly faster but **cannot be nested**.

```bash
# Correct
var fd = sys-open "/tmp/log" O_RDONLY

# INVALID - Cannot nest directly
var fd = sys-open (sys-open ...) 
```

##### Command Substitution `$(...)`
Using `$(...)` captures the output (return value) of *any* command, including user-defined functions, and allows for **nesting**.

```bash
# Capture user function return
var sum = $(add 10 20)

# Nested calls
var result = $(add 5 $(sub 10 2))

# Use in arguments
sys-write 1 $(sys-uname)
```
Atomic supports basic integer arithmetic in variable assignments:
```bash
var a = 10
var b = 20
var sum = $a + $b
var diff = $a - $b
var prod = $a * $b
var quot = $b / $a
var mod = $b % 3
```
Supported operators: `+`, `-`, `*`, `/`, `%`.

#### Conditionals

```bash
if $fd >= 0
if $fd >= 0
  sys-write 1 "opened"
  sys-close $fd
else if $fd == -2
  sys-write 1 "not found"
else
  sys-write 1 "error"
end
```

Operators: `==`, `!=`, `>`, `<`, `>=`, `<=`. Nesting is supported.
*Note: String string comparisons (e.g., `if "a" == "b"`) are fully supported.*

#### Loops

Atomic supports `while` loops:

```bash
var i = 0
while $i < 5
  sys-write 1 $i
  sys-write 1 "\n"
  var i = $i + 1
end
```

#### Functions

```bash
fn open_and_read path
  var fd = sys-open $path O_RDONLY
  if $fd >= 0
    var data = sys-read $fd 256
    var data = sys-read $fd 256
    sys-write 1 $data
    sys-close $fd
  else
    sys-write 1 "cannot open:"
    sys-write 1 $path
  end
end

open_and_read "/etc/hostname"
open_and_read "/proc/version"
[EXECUTE]
```

Functions support parameters, local variable scoping, and return values.

```bash
fn add a b
  return $a + $b
end

var res = $(add 5 10)
# res is 15
```

**Recursion** is fully supported. Local variables declared with `var` inside a function shadow outer variables, allowing for safe recursive calls.

```bash
fn fib n
  if $n <= 1
    return $n
  end
  var n1 = $n - 1
  var n2 = $n - 2
  var f1 = $(fib $n1)
  var f2 = $(fib $n2)
  return $f1 + $f2
end
```

#### Comments & Print

```bash
# This is a comment
print "Hello from Atomic"
```

### Syscall Commands (44)

#### File Operations
| Command | Usage |
|---------|-------|
| `sys-open` | `sys-open <path> <flags> [mode]` |
| `sys-read` | `sys-read <fd> <count>` |
| `sys-write` | `sys-write <fd> <data>` |
| `sys-close` | `sys-close <fd>` |
| `sys-lseek` | `sys-lseek <fd> <offset> <whence>` |
| `sys-truncate` | `sys-truncate <path> <length>` |
| `sys-dup2` | `sys-dup2 <oldfd> <newfd>` |
| `sys-pipe` | `sys-pipe` (returns read/write fds) |
| `sys-ioctl` | `sys-ioctl <fd> <req> [arg]` |

Flags: `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`. Whence: `SEEK_SET`, `SEEK_CUR`, `SEEK_END`.

#### File System
| Command | Usage |
|---------|-------|
| `sys-stat` | `sys-stat <path>` |
| `sys-getdents` | `sys-getdents <fd>` (raw dirent buffer) |
| `sys-getcwd` | `sys-getcwd` |
| `sys-chdir` | `sys-chdir <path>` |
| `sys-mkdir` | `sys-mkdir <path> [mode]` |
| `sys-unlink` | `sys-unlink <path>` |
| `sys-rename` | `sys-rename <old> <new>` |
| `sys-chmod` | `sys-chmod <path> <mode>` |
| `sys-chown` | `sys-chown <path> <uid> <gid>` |
| `sys-link` | `sys-link <target> <linkname>` |
| `sys-symlink` | `sys-symlink <target> <linkname>` |
| `sys-readlink` | `sys-readlink <path>` |
| `sys-mount` | `sys-mount <src> <tgt> <fs> <flags>` |
| `sys-umount` | `sys-umount <target>` |

#### Process Management
| Command | Usage |
|---------|-------|
| `sys-fork` | `sys-fork` |
| `sys-kill` | `sys-kill <pid> <signal>` |
| `sys-wait` | `sys-wait <pid>` |
| `sys-getpid` | `sys-getpid` |
| `sys-getuid` | `sys-getuid` |
| `sys-nice` | `sys-nice <inc>` |

Signals: `SIGTERM`, `SIGKILL`, `SIGINT`, `SIGHUP`, `SIGUSR1`, `SIGUSR2`.

#### Memory
| Command | Usage |
|---------|-------|
| `sys-mmap` | `sys-mmap <len> <prot> <flags> <fd> [offset]` |
| `sys-munmap` | `sys-munmap <addr> <len>` |
| `sys-brk` | `sys-brk [addr]` |

Prot: `PROT_READ`, `PROT_WRITE`, `PROT_EXEC`. Flags: `MAP_SHARED`, `MAP_PRIVATE`, `MAP_ANON`.

#### Networking
| Command | Usage |
|---------|-------|
| `sys-socket` | `sys-socket <domain> <type> <proto>` |
| `sys-connect` | `sys-connect <fd> <ip> <port>` |
| `sys-bind` | `sys-bind <fd> <ip> <port>` |
| `sys-listen` | `sys-listen <fd> <backlog>` |
| `sys-accept` | `sys-accept <fd>` |
| `sys-send` | `sys-send <fd> <data>` |
| `sys-recv` | `sys-recv <fd> <len>` |

Domain: `AF_INET`, `AF_INET6`, `AF_UNIX`. Type: `SOCK_STREAM`, `SOCK_DGRAM`.

#### System Info
| Command | Usage |
|---------|-------|
| `sys-uname` | `sys-uname` |
| `sys-sysinfo` | `sys-sysinfo` |

#### Utilities
| Command | Usage | Description |
|---------|-------|-------------|
| `#` | `# comment` | Comment line (ignored). |

### Example: Full Script

```bash
atomic

# Check if a file exists, create it if not
fn ensure_file path
  var fd = sys-open $path O_RDONLY
  if $fd >= 0
    sys-write 1 "exists: "
    sys-write 1 $path
    sys-write 1 "\n"
    sys-close $fd
  else
    var fd2 = sys-open $path O_WRONLY|O_CREAT|O_TRUNC 0644
    sys-write $fd2 "created by atomic"
    sys-close $fd2
    sys-write 1 "created: "
    sys-write 1 $path
    sys-write 1 "\n"
  end
end

ensure_file "/tmp/atomic_test.txt"

# Read it back
var fd = sys-open "/tmp/atomic_test.txt" O_RDONLY
var content = sys-read $fd 256
sys-write 1 $content
sys-write 1 "\n"
sys-close $fd

# Show system info
sys-uname
sys-sysinfo
[EXECUTE]
```

---

## Developer: 

 Patrick Andrew Cortez - 3rd Year Computer Engineering Student


