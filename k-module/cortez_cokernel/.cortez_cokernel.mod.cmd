savedcmd_cortez_cokernel.mod := printf '%s\n'   cortez_cokernel.o | awk '!x[$$0]++ { print("./"$$0) }' > cortez_cokernel.mod
