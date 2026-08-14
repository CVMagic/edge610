# Technician guide: enabling Edge 610 front-panel Ethernet

## 1. Scope and recovery preparation

These instructions expose the six RJ45 ports attached to the Marvell 88E6190.
They do not prescribe a network topology or IP configuration.

Before starting:

- obtain serial-console access;
- keep the distro kernel or another known-good kernel as a boot fallback;
- back up `/boot`, the bootloader configuration, and the eMMC partition table;
- record current files under `/etc/modprobe.d`, `/etc/modules-load.d`, and the
  distro's network configuration directories;
- expect Secure Boot to reject unsigned local modules unless they are signed
  with an enrolled key.

Do not load these modules on a different model. The code accesses fixed
Edge 610 P2SB/GPIO resources.

## 2. Identify the hardware

Run:

```sh
cat /sys/class/dmi/id/product_name
cat /sys/class/dmi/id/board_name
lspci -nn | grep -Ei '19dd|X553'
```

Expected indicators include `EDGE610`, Intel P2SB `8086:19dd`, and Intel X553
Ethernet functions. The first X553 function wired to switch CPU port 10 was
PCI `0000:03:00.0` on the tested appliance.

After `ixgbe` is loaded, determine that function's Linux name without assuming
the distro calls it `eno1`:

```sh
ls /sys/bus/pci/devices/0000:03:00.0/net
```

Use that name as `CONDUIT` below. If PCI enumeration differs, correlate the
X553 functions by PCI address and permanent MAC address. The tested conduit
MAC was the first of the four consecutive X553 MAC addresses.

## 3. Kernel requirements

Inspect `/proc/config.gz`, `/boot/config-$(uname -r)`, or the distro kernel
configuration. Required capabilities are:

```text
CONFIG_NET_DSA
CONFIG_NET_DSA_MV88E6XXX
CONFIG_NET_DSA_TAG_DSA
CONFIG_IXGBE=m
CONFIG_I2C
CONFIG_I2C_ISMT
CONFIG_I2C_CHARDEV
CONFIG_PCI
CONFIG_SYSFS
```

The DSA and Marvell options may be built in or modular. `ixgbe` should be
modular because `edge610_switch_standalone` must change the X553 board selector
before the NIC driver probes. If the distro builds `ixgbe=y`, build a kernel
with `CONFIG_IXGBE=m` or integrate the board initializer into an earlier kernel
init stage.

Headers for the exact running kernel must provide
`linux/platform_data/mv88e6xxx.h`. If a future kernel removes or changes the
legacy platform-data API, `edge610_dsa.c` must be ported to the replacement
software-node/fwnode API.

Optional bridge, VLAN, bonding, nftables, and routing features depend on the
technician's intended network design, not this board-support layer.

## 4. Typical build prerequisites

Package names vary. Common examples are:

```sh
# Debian/Ubuntu
apt install build-essential dkms linux-headers-$(uname -r) i2c-tools iproute2

# Fedora/RHEL family (enable a DKMS provider when DKMS is desired)
dnf install gcc make kernel-devel-$(uname -r) i2c-tools iproute

# Arch Linux
pacman -S --needed base-devel dkms linux-headers i2c-tools iproute2

# openSUSE
zypper install -t pattern devel_kernel
zypper install dkms i2c-tools iproute2
```

Use the matching flavor-specific headers for custom, realtime, LTS, or vendor
kernels.

## 5. Build and install the modules

### DKMS method

From the unpacked kit:

```sh
version=1.0.0
install -d /usr/src/edge610-board-support-$version
install -m 0644 src/Makefile src/dkms.conf src/*.c \
  /usr/src/edge610-board-support-$version/
dkms add -m edge610-board-support -v $version
dkms build -m edge610-board-support -v $version -k $(uname -r)
dkms install -m edge610-board-support -v $version -k $(uname -r)
```

### Direct build method

```sh
make -C src KERNELRELEASE=$(uname -r) \
  KERNEL_DIR=/lib/modules/$(uname -r)/build
install -D -m 0644 src/edge610_switch_standalone.ko \
  /lib/modules/$(uname -r)/extra/edge610_switch_standalone.ko
install -D -m 0644 src/edge610_dsa.ko \
  /lib/modules/$(uname -r)/extra/edge610_dsa.ko
depmod -a
make -C src clean
```

DKMS is preferred when the distro regularly changes kernels. Always test a
new kernel from the serial console before removing the old one.

## 6. Module configuration and initramfs ordering

Copy and edit the reference configuration:

```sh
install -m 0644 config/edge610-switch.conf /etc/modprobe.d/
install -m 0644 config/edge610-watchdog.conf /etc/modprobe.d/
install -m 0644 config/edge610-i2c.modules \
  /etc/modules-load.d/edge610-i2c.conf
install -m 0644 config/edge610-hardware.conf /etc/
```

If the X553 conduit is not `eno1`, change both:

```text
/etc/edge610-hardware.conf: CONDUIT=<actual-name>
/etc/modprobe.d/edge610-switch.conf:
  options edge610_dsa conduit_name=<actual-name> force_cpu_phyctl=0x203e
```

The `blacklist ixgbe` directive must be reflected in the initramfs, otherwise
the distro may bind ixgbe before the root filesystem is mounted. Refresh it
using the distro's normal command, for example:

```sh
update-initramfs -u          # Debian/Ubuntu
dracut --force              # Fedora/RHEL/openSUSE family
mkinitcpio -P               # Arch Linux
```

Verify the generated initramfs includes the modprobe configuration when the
distro provides an inspection tool.

## 7. Watchdog handling

Install the reference tools:

```sh
install -m 0755 scripts/edge610-detect /usr/local/sbin/
install -m 0755 scripts/edge610-disable-watchdog /usr/local/sbin/
```

Run once and verify both controls:

```sh
/usr/local/sbin/edge610-disable-watchdog
```

The script discovers the iSMT bus dynamically; do not assume it is always
`i2c-0` or `i2c-1`. Equivalent manual writes are:

```sh
i2cset -y <ismt-bus> 0x22 0x00 0x00 b
i2cset -y <ismt-bus> 0x24 0x00 0x00 b
```

Both operations must occur every boot, before lengthy network initialization.

## 8. Bring-up order

For a manual test, substitute the actual conduit name:

```sh
CONDUIT=eno1
modprobe i2c-ismt
modprobe i2c-dev
/usr/local/sbin/edge610-disable-watchdog
modprobe edge610_switch_standalone
modprobe ixgbe
ip addr flush dev "$CONDUIT"
ip link set "$CONDUIT" up
modprobe mv88e6xxx
modprobe edge610_dsa conduit_name="$CONDUIT" force_cpu_phyctl=0x203e
for port in ge1 ge2 ge3 ge4 ge5 ge6; do ip link set "$port" up; done
```

The conduit must not receive an IP address or be added to a bridge. Configure
only the `ge*` DSA user interfaces with the distro's network tools.

### Optional systemd integration

The supplied units encode the tested ordering:

```sh
install -m 0755 scripts/edge610-switch-up scripts/edge610-verify \
  /usr/local/sbin/
install -m 0644 systemd/*.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable edge610-watchdog-disable.service edge610-switch.service
systemctl start edge610-watchdog-disable.service
systemctl start edge610-switch.service
```

For OpenRC, runit, s6, or another init system, translate the same ordering into
native service definitions. The scripts do not otherwise depend on systemd.

## 9. Verification

```sh
lsmod | grep -E 'edge610|mv88e6|ixgbe|i2c_ismt|i2c_dev'
ip -br link
ethtool ge1
dmesg | grep -Ei 'edge610|88E6190|mv88e6|DSA|ixgbe'
/usr/local/sbin/edge610-verify
```

Expected results:

- upstream `mv88e6xxx` identifies Marvell 88E6190 revision 1;
- `DSA: tree 0 setup` appears;
- `ge1@<conduit>` through `ge6@<conduit>` exist;
- CPU port 10 logs `status=0x0e4a` and `phyctl=0x203e`;
- the conduit reports 1000 Mb/s, full duplex, and link detected;
- both watchdog registers read `0x00`.

A disconnected front port normally shows `NO-CARRIER`. Connect a known-good
peer and then configure that `ge*` interface using the distro network stack.

## 10. Troubleshooting

### Front interfaces never appear

- Confirm the kernel has DSA, `mv88e6xxx`, and DSA tag support.
- Confirm `edge610_dsa` loaded after `ixgbe` and the conduit exists.
- Inspect `dmesg` for MDIO registration or platform-data errors.
- Confirm the module was built against the exact running kernel.

### Copper carrier is present but DHCP/ARP receives nothing

Inspect the CPU-port line in `dmesg`. `phyctl=0x0003` and status `0x004a`
indicate that DSA reset the board's forced CPU-port mode. The fixed module must
restore `force_cpu_phyctl=0x203e`, yielding status `0x0e4a`.

### X553 conduit has no carrier

- Ensure `edge610_switch_standalone` loaded before `ixgbe`.
- Ensure the initramfs honors the ixgbe blacklist.
- Confirm `x553_sgmii=1`, `cpu_port=10`, and `cpu_mac_ctl=0x203e`.
- Check for the log showing X553 `NW_MNG_IF_SEL` changed to `0x02082002`.

### Module signature or lockdown failure

Sign both modules using the distro's supported Secure Boot/MOK workflow, or
boot with module signature enforcement disabled according to local policy.

### `invalid module format` or missing symbols

Remove the build output and rebuild against `/lib/modules/$(uname -r)/build`.
Vendor kernels may change or withhold internal DSA symbols; use that vendor's
matching development package or integrate the module source into its kernel
tree.

### Watchdog service cannot find the bus

Confirm `i2c-ismt` loaded and inspect:

```sh
for adapter in /sys/bus/i2c/devices/i2c-*; do
  printf '%s: ' "$adapter"; cat "$adapter/name"
done
```

## 11. Removal and rollback

From serial console, remove network configuration referring to `ge1`-`ge6`,
then disable any added init services. Remove the DKMS module if used:

```sh
dkms remove -m edge610-board-support -v 1.0.0 --all
```

Remove the copied service, script, modules-load, and modprobe files; refresh
the initramfs so ixgbe is no longer blacklisted; then reboot into the retained
fallback kernel. Do not unload the DSA modules while a remote session depends
on a front-panel port.

