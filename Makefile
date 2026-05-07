.PHONY: all kmod user clean check install-hooks

HOOK_SOURCES := \
	scripts/common.sh \
	scripts/install-git-hooks.sh \
	scripts/pre-commit.hook \
	scripts/commit-msg.hook \
	scripts/prepare-commit-msg.hook

HOOK_STAMP := .hooks-installed

all: | $(HOOK_STAMP)
all: kmod user

$(HOOK_STAMP): $(HOOK_SOURCES)
	bash scripts/install-git-hooks.sh
	touch $(HOOK_STAMP)

kmod:
	make -C kmod

user:
	make -C user

clean:
	make -C user clean
	make -C kmod clean

check:
	sudo bash scripts/check.sh

install-hooks:
	bash scripts/install-git-hooks.sh
	touch $(HOOK_STAMP)
