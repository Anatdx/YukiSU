KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

ifneq ($(KDIR),)
$(info -- KDIR: $(KDIR))
$(info -- MDIR: $(MDIR))
endif

.PHONY: all compdb clean format check-format

all: check_symbol
	make -C $(KDIR) M=$(MDIR) modules
	./check_symbol kernelsu.ko $(KDIR)/vmlinux
compdb:
	python3 $(MDIR)/.vscode/generate_compdb.py -O $(KDIR) $(MDIR)
clean:
	make -C $(KDIR) M=$(MDIR) clean
	rm check_symbol
check_symbol: tools/check_symbol.c
	$(CC) tools/check_symbol.c -o check_symbol
format:
	git -C $(MDIR) ls-files -z -- "*.c" "*.h" | xargs -0 clang-format -i
check-format:
	git -C $(MDIR) ls-files -z -- "*.c" "*.h" | xargs -0 clang-format --dry-run --Werror

# Keep a new line here!! Because someone may append config
