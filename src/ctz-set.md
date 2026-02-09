# Cortez Set Configuration Format (`.set`)

**Set** is a powerful, human-readable configuration format designed for the Cortez Terminal project. It combines the simplicity of YAML/JSON with advanced features like dynamic evaluation, references, and strong typing.

## 1. Syntax Guide

### Basic Types

```text
# Strings (Double or Single quotes)
name: "Cortez"
type: 'Terminal'

# Integers
version: 2
max_threads: 16

# Floats
opacity: 0.95
scale: 1.5

# Booleans
enabled: true
debug: false
# Also supports: on/off, yes/no

# Null
custom_path: null
```

### Collections

**Maps** (Key-Value pairs):
```text
server:
  host: "localhost"
  port: 8080
```

**Arrays** (Ordered lists):
```text
ports: [80, 443, 8080]
users: ["admin", "guest"]
```

**Blocks** (Alternative Map Syntax):
Useful for nested sections without indentation reliance.
```text
network -:
  interface: "eth0"
  dhcp: true
:-
```

### Advanced Features

#### Multi-line Strings
Preserve newlines and formatting.
```text
description: '''
Welcome to Cortez.
This is a multi-line description.
'''
```

#### Raw Strings
Treat backslashes literally (great for Windows paths or Regex).
```text
path: r"C:\Windows\System32"
regex: r"^\d{3}-\d{2}-\d{4}$"
```

#### Decorators
Annotate fields with metadata flags.
```text
@private
api_key: "sk_live_..."

@deprecated
legacy_mode: true

@readonly
system_id: 101
```

#### Anchors & Aliases
Define a value once (`&name`) and reuse it (`*name`).
```text
# Define a base configuration
base_theme: &theme_defaults
  font: "Fira Code"
  size: 12

# Reuse it (Shallow Copy)
profile_1: *theme_defaults
profile_2: *theme_defaults
```

#### Namespaced Includes
Import other `.set` files into a specific namespace.
```text
# Imports content of 'colors.set' into 'theme' map
include "colors.set" as theme

# Access: theme.primary, theme.background
```

#### Dynamic Evaluation

**Variable Interpolation**:
```text
app_name: "Cortez"
title: "Welcome to ${app_name}"
```

**Environment Variable Fallbacks**:
```text
# Use $PORT env var, or 8080 if not set
port: "${PORT:-8080}"
```

**Computed Expressions**:
Perform basic arithmetic (`+`, `-`, `*`, `/`, `()`).
```text
width: 1920
center_x: $(width / 2)
aspect: $(16 / 9)
```
*Note: Expressions can be quoted `"${...}"` or bare `$(...)`.*

---

## 2. C API Usage

The `ctz-set` library provides a robust C API for loading, querying, and modifying `.set` files.

### Include
```c
#include "ctz-set.h"
```

### Loading & Lifecycle

```c
// Load a file
SetConfig* cfg = set_load("config.set");
if (!cfg) {
    printf("Error: %s\n", set_get_error(NULL)); // or check global error
    return 1;
}

// ... use config ...

// Free memory
set_free(cfg);
```

### Accessing Data (Simplified)

Use the `set_get_*` helper functions for easy access.

```c
// Get String (with default)
const char* name = set_get_string(cfg, "server", "host", "localhost");

// Get Integer
long port = set_get_int(cfg, "server", "port", 80);

// Get Boolean
int debug = set_get_bool(cfg, NULL, "debug", 0);
```

### Accessing Data (Node API)

For more control (arrays, iteration, flags), use the Node API.

```c
// Get Root Node
SetNode* root = set_get_root(cfg);

// Navigate
SetNode* users = set_get_child(root, "users");

// Iterate Array
if (set_node_type(users) == SET_TYPE_ARRAY) {
    size_t count = set_node_size(users);
    for (size_t i = 0; i < count; i++) {
        SetNode* item = set_get_at(users, i);
        printf("User: %s\n", set_node_string(item, ""));
    }
}

// Check Flags
SetNode* api_key = set_get_child(root, "api_key");
uint32_t flags = set_node_flags(api_key);
if (flags & 1) printf("This is private!\n"); // 1=Private, 2=Deprecated, 4=ReadOnly
```

### Modifying Data

```c
// Set a value
SetNode* root = set_get_root(cfg);
set_set_child(root, "version", SET_TYPE_INT);
SetNode* ver = set_get_child(root, "version");
set_node_set_int(ver, 3);

// Save changes
set_save(cfg); // Overwrites original file
// OR
set_dump(cfg, stdout); // Print to stream
```

### Error Handling

```c
const char* err = set_get_error(cfg);
if (err) {
    fprintf(stderr, "Set Error: %s\n", err);
}
```
