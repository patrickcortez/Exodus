savedcmd_exodus_console.mod := printf '%s\n'   exodus_console.o | awk '!x[$$0]++ { print("./"$$0) }' > exodus_console.mod
