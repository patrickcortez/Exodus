# Exodus

This is **exodus coordinator** which acts as the name suggests for units across your network.

---

## Set up

### 1. Compile Coordinator

``` bash

gcc -Wall -Wextra -O2 exodus-coordinator.c ../ctz-json.a -o ../s-bin/exodus-coordinator -pthread

```

### 2. Quick-Start

``` bash

./exodus-coordinator

```

Just make sure you're running the **Coordinator** on a seperate device (e.g: server,pc or laptop) with a stable internet connection.

---



## Clean up

``` bash

make clean

```