# Edge 610 Linux board-support notes and modules

This kit documents and packages the board-specific work needed to expose the
six front-panel Marvell 88E6190 ports on a VMware/Dell Edge 610 under Linux.
It is intentionally **not a network configuration package**. After successful
bring-up, the operating system has six independent DSA interfaces named
`ge1` through `ge6`. Technicians should configure addressing, VLANs, bridges,
bonds, firewalling, and routing with their distro's normal tools.

Version 1.0.0 was validated on x86-64 with Linux
`6.12.94-edge610-lite`. The modules use upstream kernel APIs, but out-of-tree
modules can require small source adjustments when kernel APIs change.

## What is board-specific

The Edge 610 does not describe its switch topology to Linux through ACPI or a
device tree. Two modules fill that gap:

- `edge610_switch_standalone` bit-bangs the board MDIO pins, releases and
  identifies the 88E6190, selects the X553's working 1 Gb/s SGMII path, and
  performs the early CPU-port setup before `ixgbe` probes.
- `edge610_dsa` registers the relocated MDIO bus and the topology expected by
  upstream `mv88e6xxx`: switch ports 1-6 become `ge1`-`ge6`, and switch port
  10 connects to the first X553 function (called `eno1` in the tested system).

Upstream DSA resets CPU-port physical control during setup. The board module
therefore restores register `0x01` to `0x203e`; without this, copper carrier
may be visible but no packets cross the switch CPU link.

The board also contains watchdog controls at I2C addresses `0x22` and `0x24`
on the Intel iSMT adapter. Both must be written to zero early in boot.

## Bundle layout

- `src/`: GPL-2.0 kernel-module sources, Makefile, and optional DKMS metadata.
- `scripts/`: hardware detection, watchdog disable, ordered switch bring-up,
  and verification helpers.
- `systemd/`: optional reference units for the required boot order.
- `config/`: reference modprobe, modules-load, and conduit-name configuration.
- `docs/TECHNICIAN-GUIDE.md`: full distro-neutral integration procedure.

## Quick manual outline

1. Confirm the system identifies as `EDGE610` and contains PCI device
   `8086:19dd`.
2. Confirm the running kernel contains the required DSA, Marvell, ixgbe,
   iSMT, and I2C features. In particular, `ixgbe` must be a module so the
   board initializer can run first.
3. Build `src/` against the exact running-kernel headers, either directly or
   with DKMS.
4. Blacklist automatic `ixgbe` probing, refresh the initramfs, and preserve
   this load order:

   ```text
   i2c-ismt/i2c-dev -> disable both watchdogs
   edge610_switch_standalone -> ixgbe -> X553 conduit up
   mv88e6xxx -> edge610_dsa -> ge1..ge6 up
   distro network configuration
   ```

5. Verify CPU port 10 reports status `0x0e4a`, physical control `0x203e`, and
   that all six `ge*` devices exist.

See [docs/TECHNICIAN-GUIDE.md](docs/TECHNICIAN-GUIDE.md) before changing a
machine. Serial console access and a known-good fallback kernel are strongly
recommended.

## Network policy

This kit creates no bridge and assigns no address. Examples of valid policies
after board bring-up include:

- a DHCP or static address directly on any one `ge*` interface;
- a Linux bridge containing selected ports;
- VLAN subinterfaces or bridge VLAN filtering;
- independent routed ports;
- bonding, subject to the normal DSA and distro constraints.

Do not assign an address to the X553 conduit itself. The conduit is the DSA
transport between the host and switch, not a front-panel network interface.

## License and support status

The kernel modules are GPL-2.0 as declared by their SPDX/module metadata.
This is community board support derived from hardware investigation; it is not
an official VMware, Dell, Broadcom, Intel, or Marvell release.

