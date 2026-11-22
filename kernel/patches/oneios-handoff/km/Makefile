# Out-of-tree build against a GKI kernel tree (android15-6.6, matching the S25's kernel 6.6.98 --
# see README.md "Which kernel source" for why that branch and not another).
# Usage (inside a checked-out GKI common kernel tree, or pass KDIR=path/to/common):
#   make -C $KDIR M=$(pwd) ARCH=arm64 CROSS_COMPILE=... LLVM=1 modules
obj-m += oneios_handoff.o

all:
	@echo "Run from .github/workflows/handoff-km.yml, or: make -C \$$KDIR M=\$$(pwd) ARCH=arm64 LLVM=1 modules"

clean:
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order
