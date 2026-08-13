// RUN: %inputgen-gpu --help | FileCheck --check-prefix=HELP %s
// RUN: %not %inputgen-gpu invalid dummy.image dummy.json 2>&1 | FileCheck --check-prefix=BAD-MODE %s
// RUN: printf '[]\n' > %t.array.json
// RUN: %not %inputgen-gpu generate dummy.image %t.array.json 2>&1 | FileCheck --check-prefix=BAD-JSON-OBJECT %s
// RUN: printf '{ "DeviceId": 0 }\n' > %t.no-name.json
// RUN: %not %inputgen-gpu generate dummy.image %t.no-name.json 2>&1 | FileCheck --check-prefix=BAD-JSON-NAME %s
// RUN: printf '{ "Name": "vvv_foo" }\n' > %t.no-device.json
// RUN: %not %inputgen-gpu generate dummy.image %t.no-device.json 2>&1 | FileCheck --check-prefix=BAD-JSON-DEVICE %s
// RUN: printf '{ "Name": "vvv_foo", "DeviceId": 0 }\n' > %t.valid.json
// RUN: %not %inputgen-gpu generate missing.image %t.valid.json 2>&1 | FileCheck --check-prefix=MISSING-IMAGE %s

// HELP: OVERVIEW: Launch an InputGen GPU entry kernel from an image and record JSON
// HELP: USAGE: llvm-inputgen-gpu
// HELP: --inputgen-data

// BAD-MODE: error: invalid mode 'invalid'; expected 'generate' or 'replay'
// BAD-JSON-OBJECT: error: invalid JSON file
// BAD-JSON-NAME: error: failed to read JSON string Name
// BAD-JSON-DEVICE: error: failed to read JSON integer DeviceId
// MISSING-IMAGE: error: failed to read image file 'missing.image'
