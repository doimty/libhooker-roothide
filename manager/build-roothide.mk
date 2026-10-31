# Optional RootHide Procursus addon; the upstream Makefile remains the rootful build.
.PHONY: all package test

all: package

package:
	bash scripts/build-roothide.sh

test:
	bash tests/run.sh
