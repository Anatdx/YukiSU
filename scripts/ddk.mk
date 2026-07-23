.PHONY: all clean compdb

# ddk mounts its current directory at /build. Run it from the repository root
# so the kernel module can see both kernel/ and the shared uapi/ headers.
all:
	$(MAKE) -C kernel

clean:
	$(MAKE) -C kernel clean

compdb:
	$(MAKE) -C kernel compdb
