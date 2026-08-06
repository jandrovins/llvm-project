// RUN: %inputgen-gpu --help | FileCheck --check-prefix=HELP %s
// RUN: %not %inputgen-gpu invalid dummy.image dummy.json 2>&1 | FileCheck --check-prefix=BAD-MODE %s

// HELP: OVERVIEW: Launch an InputGen GPU entry kernel from an image and record JSON
// HELP: USAGE: llvm-inputgen-gpu
// HELP: --inputgen-data

// BAD-MODE: error: invalid mode 'invalid'; expected 'generate' or 'replay'
