.PHONY: all kmod user clean check install-hooks

.DEFAULT_GOAL := all

HOOK_SOURCES := \
	scripts/common.sh \
	scripts/install-git-hooks.sh \
	scripts/pre-commit.hook \
	scripts/commit-msg.hook \
	scripts/prepare-commit-msg.hook

HOOK_STAMP := .hooks-installed

UNAME_S := $(shell uname -s)

$(HOOK_STAMP): $(HOOK_SOURCES)
	bash scripts/install-git-hooks.sh
	touch $(HOOK_STAMP)

install-hooks:
	bash scripts/install-git-hooks.sh
	touch $(HOOK_STAMP)

ifeq ($(UNAME_S),Linux)

all: | $(HOOK_STAMP)
all: kmod user

kmod:
	make -C kmod

user:
	make -C user

clean:
	make -C user clean
	make -C kmod clean

check:
	sudo bash scripts/check.sh

else

all: $(HOOK_STAMP)
	@echo "vpipe: non-Linux host ($(UNAME_S)); installed git hooks only."
	@echo "vpipe: run builds, modules, and validation inside a Linux host."
	@exit 1

kmod user check:
	@echo "vpipe: '$@' is Linux-only; run inside a Linux guest (e.g. lima)." >&2
	@exit 1

clean:
	make -C user clean
	make -C kmod clean

endif
