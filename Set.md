# Cortez Set Configuration (`.set`)

**Set** is a concise, typed configuration format with dynamic features, designed for the Cortez Terminal. It doubles as a lightweight, embedded database engine.

## 1. Syntax Reference

### Primitives

| Type | Syntax | Example |
| :--- | :--- | :--- |
| **String** | `"..."` or `'...'` | `name: "Cortez"` |
| **Integer** | Standard digits | `port: 8080` |
| **Float** | Decimal point or scientific | `opacity: 0.95`, `tol: 1e-5` |
| **Boolean** | `true`/`false`, `on`/`off` | `debug: true` |
| **Null** | `null` | `proxy: null` |

### Collections

**Maps** (Key-Value):
```text
server:
  host: "localhost"
  port: 8080
```

**Arrays** (Ordered List):
```text
ports: [80, 443, 8080]
users: ["admin", "guest"]
```

**Blocks** (Explicit Scope):
```text
network -:
  interface: "eth0"
  dhcp: true

:-
```

> [!IMPORTANT]
> Curly braces `{` and `}` are **reserved** for string interpolation (e.g., `${var}`) and cannot be used as standalone tokens for blocks or maps. Use indentation or `[]` instead.

### Advanced Features

#### Strings
**Multi-line**: Preserves newlines.
```text
desc: '''
Line 1
Line 2
'''
```

**Raw**: Ignores escape sequences.
```text
path: r"C:\Windows\System32"
regex: r"^\d+$"
```

#### Decorators
Metadata flags for fields.
```text
@private
key: "secret"

@readonly
id: 123
```

#### Anchors & Aliases
Reuse values (shallow copy).
```text
base: &defaults
  font: "Fira"

profile: *defaults
```

#### Namespaced Includes
Import other files.
```text
include "theme.set" as theme
# Access: theme.color
```

#### Dynamic Evaluation
**Interpolation**:
```text
root: "/var/www"
path: "${root}/html"
```

**Env Vars** (with default):
```text
port: "${PORT:-8080}"
```

**Expressions**:
```text
width: 1920
center: $(width / 2)
```

---

## 2. Use Cases

**1. Configuration**
Human-readable config files with types, comments, and dynamic values.

**2. Embedded Database**
Thread-safe, transactional storage with indexing and SQL-like querying. Ideal for local app data.

---

## 3. C API Reference

Include: `#include "ctz-set.h"`

### Basic Config

```c
// Load & Access
SetConfig* cfg = set_load("config.set");
const char* host = set_get_string(cfg, "server", "host", "localhost");
long port        = set_get_int(cfg, "server", "port", 80);

// Modify & Save
SetNode* root = set_get_root(cfg);
set_node_set_int(set_get_child(root, "version"), 2);
set_save(cfg);
set_free(cfg);
```

### Database Mode

**Lifecycle & Transactions**
```c
set_db_init(cfg);       // Enable thread-safety
set_db_lock(cfg);       // Manual locking (optional)
// ... operations ...
set_db_unlock(cfg);
set_db_commit(cfg);     // Atomic save (write-to-temp + rename)
```

**CRUD Operations**
```c
// Insert Record
SetNode* user = set_db_insert(cfg, "users"); // Creates 'users' array if missing
set_node_set_string(set_get_child(user, "name"), "Cortez");

// Select (SQL-like)
// SELECT * FROM users WHERE age > 18 LIMIT 10 OFFSET 0
SetNode* results = set_db_select(cfg, "users", "age", DB_OP_GT, (void*)18, 10, 0);
```

**Indexing (Performance)**
```c
// Create Index (B-Tree for ranges, Hash for exact)
set_index_create(cfg, "users", "email", INDEX_TYPE_HASH);
set_index_create(cfg, "users", "age",   INDEX_TYPE_BTREE);

// Query via Index
SetIndex* idx = ...; // Get index handle
SetNode* res = set_index_range(idx, (void*)18, (void*)30, 100);

// Composite Index Query
const char* fields[] = {"user_id", "status"};
set_index_create_composite(cfg, "orders", fields, 2, INDEX_TYPE_BTREE);
// ...
const void* values[] = {(void*)123, "pending"};
SetNode* orders = set_index_query_composite(idx, values, 2);
```

**Advanced Operations**
```c
// Aggregates
double avg_age = set_aggregate(cfg, "users", "age", AGG_AVG);

// Joins
// SELECT * FROM users JOIN orders ON users.id = orders.user_id
SetNode* joined = set_join(cfg, "users", "id", "orders", "user_id", JOIN_INNER);
```

---

## 4. Complete Examples

### Example 1: Configuration Loader

```c
#include "ctz-set.h"
#include <stdio.h>

int main() {
    // 1. Load Configuration
    SetConfig* cfg = set_load("app.set");
    if (!cfg) {
        fprintf(stderr, "Failed to load config: %s\n", set_get_error(NULL));
        return 1;
    }

    // 2. Read Values (with defaults)
    const char* app_name = set_get_string(cfg, "app", "name", "Unknown App");
    int max_users = set_get_int(cfg, "limits", "max_users", 100);
    int debug_mode = set_get_bool(cfg, NULL, "debug", 0);

    printf("Starting %s (Max Users: %d, Debug: %s)...\n", 
           app_name, max_users, debug_mode ? "ON" : "OFF");

    // 3. Iterate a List
    SetNode* servers = set_get_child(set_get_root(cfg), "servers");
    if (servers) {
        size_t count = set_node_size(servers);
        for (size_t i = 0; i < count; i++) {
            printf("Server %zu: %s\n", i, set_node_string(set_get_at(servers, i), ""));
        }
    }

    // 4. Cleanup
    set_free(cfg);
    return 0;
}
```

### Example 2: Embedded Database

```c
#include "ctz-set.h"
#include <stdio.h>

int main() {
    // 1. Initialize DB
    SetConfig* db = set_load("data.db");
    if (!db) db = set_create("data.db"); // Create if missing
    
    set_db_init(db); // Enable thread-safety

    // 2. Create Index (One-time setup)
    set_index_create(db, "users", "email", INDEX_TYPE_HASH);

    // 3. Insert Data
    set_db_lock(db);
    SetNode* user = set_db_insert(db, "users");
    set_node_set_string(set_get_child(user, "name"), "Cortez");
    set_node_set_string(set_get_child(user, "email"), "admin@cortez.com");
    set_node_set_int(set_get_child(user, "role_id"), 1);
    set_db_unlock(db);

    // 4. Query Data
    // Find user with specific email
    SetIndex* idx = set_index_create(db, "users", "email", INDEX_TYPE_HASH); // Get handle
    SetNode* result = set_index_query(idx, DB_OP_EQ, "admin@cortez.com", 1);

    if (result) {
        printf("Found User: %s\n", set_node_string(set_get_child(result, "name"), ""));
    }

    // 5. Commit Changes
    if (set_db_commit(db) != 0) {
        fprintf(stderr, "Commit failed: %s\n", set_get_error(db));
    }

    set_free(db);
    return 0;
}
```

