# cxl-extras

Out-of-tree kernel module and test application for QEMU's emulated
CXL type-2 accelerator device. Useful for kernel CXL core
development without hardware and as a test platform for CXL type-2
device passthrough via vfio-pci/vfio-cxl.

## cxl-type2

PCI driver for the QEMU emulated CXL type-2 accelerator device
(`cxl-accel`). It uses the kernel CXL core infrastructure to expose
the device memory through the standard DAX path, and provides a
command interface for offloading work to the accelerator.

```
L0 host
  No CXL hardware required

L1 QEMU
  cxl-accel device with CXL.mem + command BAR
  └── Emulated type-2 accelerator (vendor 1b36:0123)

L1 kernel
  cxl_type2 driver
  DAX device for CXL memory access
  exposes /dev/cxl-accelN for command submission
```

### Unified memory

The driver registers a CXL memory device so the kernel discovers the
CXL region and creates a DAX device (`/dev/daxN.M`). The memory can
be used as a devdax character device for direct accelerator access,
or onlined as system RAM via `daxctl reconfigure-device --mode=system-ram`.
Accelerator commands require devdax mode since they operate on DPA
offsets within the mmap'd device memory.

### Command BAR

The accelerator exposes MMIO registers on PCI BAR 5. The BAR starts
with a read-only capability header for device discovery, followed by
the command submission registers. The driver reads the capability
header at probe time to discover supported operations, maximum
transfer size, and the offset to the command registers.

Command submission flow:

```
userspace                    kernel driver              QEMU device
    |                             |                          |
    |-- ioctl(SUBMIT, cmd) ------>|                          |
    |                             |-- write MMIO regs ------>|
    |                             |-- ring doorbell -------->|
    |                             |                          |-- process
    |                             |<--- MSI-X interrupt -----|
    |                             |-- read status/result --->|
    |<-- return result_len -------|                          |
```

### Capability registers

| Offset | Size | Name       | R/W | Description                        |
|--------|------|------------|-----|------------------------------------|
| 0x00   | 4    | MAGIC      | RO  | 0x32584C43 ("CLX2")                |
| 0x04   | 4    | VERSION    | RO  | Interface version (currently 1)    |
| 0x08   | 4    | CAPS       | RO  | Bitmask of supported opcodes       |
| 0x0C   | 4    | MAX_XFER   | RO  | Maximum transfer size in bytes     |

### Command registers (at offset 0x10)

| Offset | Size | Name       | R/W | Description                        |
|--------|------|------------|-----|------------------------------------|
| 0x00   | 4    | OPCODE     | RW  | Command opcode                     |
| 0x04   | 4    | -          | -   | Reserved (alignment padding)       |
| 0x08   | 8    | SRC_OFFSET | RW  | Source DPA offset in device memory  |
| 0x10   | 8    | SRC_LENGTH | RW  | Source data length in bytes         |
| 0x18   | 8    | DST_OFFSET | RW  | Destination DPA offset             |
| 0x20   | 8    | DST_LENGTH | RW  | Destination buffer length          |
| 0x28   | 4    | DOORBELL   | WO  | Write 1 to submit command          |
| 0x2C   | 4    | -          | -   | Reserved (alignment padding)       |
| 0x30   | 4    | STATUS     | RO  | 0=idle, 2=done, 3=error            |
| 0x34   | 4    | -          | -   | Reserved (alignment padding)       |
| 0x38   | 8    | RESULT_LEN | RO  | Output data length after completion|

### Opcodes

| Value | Name       | Description                                      |
|-------|------------|--------------------------------------------------|
| 1     | COMPRESS   | zlib-compress src region into dst region          |
| 2     | DECOMPRESS | zlib-decompress src region into dst region        |

The CAPS register encodes supported opcodes as a bitmask: bit N is
set if opcode N is supported. The driver validates each ioctl
submission against CAPS and rejects transfers exceeding MAX_XFER.

### Userspace ioctl

```c
struct cxl_accel_cmd {
    __u32 opcode;
    __u32 reserved;
    __u64 src_offset;   /* DPA offset of input data  */
    __u64 src_length;
    __u64 dst_offset;   /* DPA offset of output buffer */
    __u64 dst_length;
    __u64 result_len;   /* returned: actual output size */
};

#define CXL_ACCEL_IOCTL_SUBMIT  _IOWR('C', 0x01, struct cxl_accel_cmd)
```

The ioctl is blocking with a 5-second timeout. Source and destination
offsets refer to DPA addresses within the CXL device memory, which
userspace accesses by mmapping the corresponding `/dev/daxN.M` device.

## cxl_accel_test

Userspace test application that exercises the compress/decompress
roundtrip:

1. Opens `/dev/daxN.M` and mmaps CXL device memory
2. Writes input data (file or synthetic pattern)
3. Submits COMPRESS via ioctl, then DECOMPRESS
4. Verifies the decompressed output matches the original

The test uses fixed DPA offsets starting at 0, so only one instance
should run at a time. Parallel runs will corrupt each other's data
regions in device memory.

```sh
# Check CXL topology and DAX devices
cxl list
daxctl list

# Ensure the DAX device is in devdax mode (not system-ram)
daxctl reconfigure-device dax0.0 --mode=devdax --force

# Default: 4 KB pattern, /dev/cxl-accel0, /dev/dax0.0
cxl_accel_test

# Compress a file
cxl_accel_test /path/to/file

# Custom devices and pattern size
cxl_accel_test -a /dev/cxl-accel1 -d /dev/dax1.0 -s 64
```

## QEMU

The emulated CXL type-2 accelerator device is available at
https://github.com/legoater/qemu/commits/cxl-type2/

Sample command line for a q35 machine with a CXL type-2 device:

```sh
qemu-system-x86_64 \
  -object memory-backend-ram,id=vmem0,size=256M \
  -device pxb-cxl,bus=pcie.0,id=cxl.0,bus_nr=52,addr=0x3,hdm_for_passthrough=true \
  -device cxl-rp,bus=cxl.0,id=rp0,port=0 \
  -device cxl-accel,bus=rp0,volatile-memdev=vmem0 \
  -M q35,cxl=on,cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=512M \
  ...
```

`hdm_for_passthrough=true` keeps the host bridge HDM decoder
capability visible so the kernel discovers and uses it. Without
it, QEMU hides the HB decoder and the kernel falls back to
passthrough mode (single-port, no decoder programming needed).
QEMU pre-programs the HB decoder during reset so the CXL region
is already committed when the kernel enumerates.

### Limitations

Current limitations:

- Single HDM decoder only (no interleaving)
- Single target per CFMWS window
- Synchronous command execution (doorbell blocks QEMU vCPU thread)
- No CXL error injection or RAS support
- No multi-device topology

## QEMU nested scenario

The same driver works in a nested virtualization setup where the
CXL type-2 device is passed through from L1 to L2 via vfio-pci.

This requires the following series (not yet upstream):

Kernel:
- [\[v12 00/12\] PCI/CXL: Add CXL reset support for Type 2 devices (Srirangan Madhavan)](https://lore.kernel.org/all/20260910070808.1444264-1-smadhavan@nvidia.com)
- [\[v5 00/27\] vfio/pci: Add CXL Type-2 device passthrough support (Manish Honap)](https://lore.kernel.org/all/20260916183540.3813685-1-mhonap@nvidia.com)
- [Pre-built Linux tree with all patches applied](https://github.com/legoater/linux/commits/vfio/)

QEMU:
- [\[PATCH v2 00/10\] QEMU: CXL Type-2 device passthrough via vfio-pci (Manish Honap)](https://lore.kernel.org/all/20260916184412.3825713-1-mhonap@nvidia.com/)
- Extra patches: HB HDM decoder commit in pxb-cxl for auto-region, `hdm_for_passthrough=true` requirement, DVSEC range base fix, and build stubs
- [Pre-built QEMU tree with all patches applied](https://github.com/legoater/qemu/commits/vfio-11.2)

```
L0 host
  No CXL hardware required

L1 QEMU
  cxl-accel device with CXL.mem + command BAR
  └── Emulated type-2 accelerator (vendor 1b36:0123)

L1 kernel
  cxl_type2 driver (or vfio-pci for passthrough)

L1 QEMU (nested, with vfio-pci passthrough)
  passes the type-2 device to L2

L2 kernel
  cxl_type2 driver (same driver)
  DAX memory + /dev/cxl-accelN, fully functional
```

### Prerequisites

The L1 kernel must be built with `CONFIG_CXL_BUS`, `CONFIG_CXL_MEM`,
`CONFIG_VFIO_PCI` and `CONFIG_VFIO_CXL`. Ensure the required modules are loaded before
binding:

```sh
modprobe vfio-pci
modprobe vfio-cxl
```

### Bind the device

Bind the device to vfio-pci on L1:

```sh
# Identify the device BDF (e.g. 0000:35:00.0)
lspci -d 1b36:0123

# Unbind from cxl_type2 and bind to vfio-pci
echo 0000:35:00.0 > /sys/bus/pci/devices/0000:35:00.0/driver/unbind
echo vfio-pci > /sys/bus/pci/devices/0000:35:00.0/driver_override
echo 0000:35:00.0 > /sys/bus/pci/drivers/vfio-pci/bind
```

Verify that vfio-cxl attached to the device (use count must be
non-zero). A use count of 0 means the CXL init failed and the device
is driven as plain vfio-pci without CXL passthrough support:

```sh
lsmod | grep vfio_cxl
# vfio_cxl  20480  1    <-- 1 means CXL init succeeded
```

### Start the L2 guest

Start the L2 guest with the passthrough device:

```sh
qemu-system-x86_64 \
  -object iommufd,id=iommufd0 \
  -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1,hdm_for_passthrough=true \
  -device cxl-rp,port=0,bus=cxl.1,id=cxlrp0,chassis=0,slot=2,pref64-reserve=2G,mem-reserve=1G \
  -device vfio-pci-nohotplug,host=0000:35:00.0,bus=cxlrp0,id=cxldev0,iommufd=iommufd0 \
  -M q35,cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G,cxl=on \
  ...
```

Inside the L2 guest, the same [cxl_accel_test](#cxl_accel_test) procedure
applies: load the `cxl_type2` driver, reconfigure the DAX device, and
run the test.

### Kernel selftest

The vfio CXL Type-2 selftest validates the passthrough interface on
L1 before starting the L2 guest. Build and run from the kernel tree:

```sh
make headers_install
make -C tools/testing/selftests/vfio

# Run with the device bound to vfio-pci
sudo ./tools/testing/selftests/vfio/vfio_cxl_type2_test 0000:35:00.0
```

The test verifies CXL device flags, HDM memory mmap, component
register access, decoder state, DVSEC virtualization and device reset.

## Build

```sh
make KDIR=/path/to/linux
sudo make modules_install KDIR=/path/to/linux
```

Defaults to the running kernel if `KDIR` is not set. Requires
kernel v7.3 or later (for `cxl/pci.h` and `cxl_set_capacity`).
The userspace test application is built alongside the module.

## License

GPL-2.0-only
