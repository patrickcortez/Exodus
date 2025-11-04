savedcmd_cortez_tunnel.mod := printf '%s\n'   cortez_tunnel.o | awk '!x[$$0]++ { print("./"$$0) }' > cortez_tunnel.mod
