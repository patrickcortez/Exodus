# Exodus

**Exodus** is a Real Time Multi Daemon File System Monitoring & Version Control (C/Linux) that
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

### 5. Starting Exodus

``` bash

exodus start

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
  - change:       Find and replace a word in the last indexed file
  - wc:           Get the word count of the last indexed file
  - wl:           Get the line count of the last indexed file
  - cc:           Get the non-space character count of the last indexed file

[Unit and Network Utility/Management]
  - unit-list:    List all connected Units on the network
  - view-unit :    List all nodes on a specific remote Unit
  - sync:         Sync history with a remote node (e.g., sync <unit> <remote-node> <local-node>)
  - unit-set:     Set this machine's name or coordinator (--name, --coord)
  - view-cache    For debugging, views the local node list of the signal daemon, to make sure it's upto date.

---

## Concepts and Definitions

- Node: A directory on your filesystem that Exodus is tracking (e.g., /home/user/projects/my-site). This is the root of a versioned project.

- Unit: A Device/Machine that has Exodus.

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

## Developer: 

### Patrick Andrew Cortez


