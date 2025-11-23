.PHONY: format format-check

format:
	@clang-format -i -style=file ${ALL_SRCS}

ALL_SRCS=$(shell find . -name "*.h" -o -name "*.cc")

format-check:
	@ if ! diff -u <(cat ${ALL_SRCS}) <(clang-format -style=file ${ALL_SRCS}); then \
		echo "Code format check failed. Please run 'make format' to fix it."; \
		exit 1; \
	fi
