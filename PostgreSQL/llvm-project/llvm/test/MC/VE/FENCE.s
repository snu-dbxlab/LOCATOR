# RUN: llvm-mc -triple=ve --show-encoding < %s \
# RUN:     | FileCheck %s --check-prefixes=CHECK-ENCODING,CHECK-INST
# RUN: llvm-mc -triple=ve -filetype=obj < %s | llvm-objdump -d - \
# RUN:     | FileCheck %s --check-prefixes=CHECK-INST

# CHECK-INST: fencei
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x20]
fencei

# CHECK-INST: fencem 1
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x20]
fencem 1

# CHECK-INST: fencem 2
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x20]
fencem 2

# CHECK-INST: fencem 3
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x20]
fencem 3

# CHECK-INST: fencec 1
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x20]
fencec 1

# CHECK-INST: fencec 2
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x20]
fencec 2

# CHECK-INST: fencec 3
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x20]
fencec 3

# CHECK-INST: fencec 4
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x20]
fencec 4

# CHECK-INST: fencec 5
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x05,0x00,0x20]
fencec 5

# CHECK-INST: fencec 6
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x20]
fencec 6

# CHECK-INST: fencec 7
# CHECK-ENCODING: encoding: [0x00,0x00,0x00,0x00,0x00,0x07,0x00,0x20]
fencec 7