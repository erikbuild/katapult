#!/bin/bash
# ABOUTME: Builds and runs the host unit tests in test/unit/ with the host C compiler.
# ABOUTME: Exits non-zero if a test fails to build or reports a failure.
set -eu

OUTDIR=out/unit
mkdir -p ${OUTDIR}
for TEST in test/unit/*_test.c ; do
    BIN=${OUTDIR}/$(basename ${TEST} .c)
    ${HOSTCC:-cc} -Wall -Werror -O2 -Isrc -o ${BIN} ${TEST}
    ${BIN}
done
