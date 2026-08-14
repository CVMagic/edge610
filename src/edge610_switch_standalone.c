#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/bitops.h>

#define DRV "edge610-switch"
#define P2SB_DEVICE_ID 0x19dd
#define NORTH_OFFSET 0x00c20000
#define NORTH_SIZE 0x10000
#define PIN_RESET 1
#define PIN_MDC 33
#define PIN_MDIO 34

#define PADCFG0_TXSTATE BIT(0)
#define PADCFG0_RXSTATE BIT(1)
#define PADCFG0_TXDIS BIT(8)
#define PADCFG0_RXDIS BIT(9)
#define PADCFG0_PMODE GENMASK(12, 10)

#define PORT_MAC_CTL 0x01
#define PORT_SWITCH_ID 0x03
#define PORT_CTL0 0x04
#define PORT_BASE_VLAN 0x06
#define PORT_DEFAULT_VLAN 0x07
#define PORT_CTL2 0x08

#define PORT_STATE_MASK 0x0003
#define PORT_STATE_FORWARDING 0x0003
#define PORT_FRAME_MODE_MASK 0x0300
#define PORT_8021Q_MODE_MASK 0x0c00

#define GLOBAL2 0x1c
#define G2_SMI_CMD 0x18
#define G2_SMI_DATA 0x19
#define G2_BUSY 0x8000
#define G2_C45_WRITE_DATA 0x0400
#define G2_C45_READ_DATA 0x0c00
#define MDIO_MMD_PHYXS 4
#define SERDES_BMCR 0x2000
#define SERDES_BMSR 0x2001
#define SERDES_ADVERTISE 0x2004
#define SERDES_LPA 0x2005
#define SERDES_PHY_STATUS 0xa003
#define X553_NW_MNG_IF_SEL 0x11178

static void __iomem *north;
static void __iomem *pads;
static ushort cpu_mac_ctl = 0x303f;
module_param(cpu_mac_ctl, ushort, 0444);
MODULE_PARM_DESC(cpu_mac_ctl, "88E6190 port 10 MAC control value");
static bool serdes_autoneg;
module_param(serdes_autoneg, bool, 0444);
MODULE_PARM_DESC(serdes_autoneg, "Enable port 10 2.5GBASE-X PCS autonegotiation");
static ushort cpu_port = 10;
module_param(cpu_port, ushort, 0444);
MODULE_PARM_DESC(cpu_port, "88E6190 CPU port: 9 (alternate) or 10 (primary)");
static bool x553_sgmii;
module_param(x553_sgmii, bool, 0444);
MODULE_PARM_DESC(x553_sgmii, "Temporarily select X553 SGMII/1G mode");

static void __iomem *padcfg(unsigned int pin)
{
	return pads + pin * 8;
}

static void pad_input(unsigned int pin)
{
	void __iomem *cfg = padcfg(pin);
	u32 value = readl(cfg);

	value &= ~PADCFG0_PMODE;
	value &= ~PADCFG0_RXDIS;
	value |= PADCFG0_TXDIS;
	writel(value, cfg);
	readl(cfg);
}

static void pad_output(unsigned int pin, int high)
{
	void __iomem *cfg = padcfg(pin);
	u32 value = readl(cfg);

	value &= ~PADCFG0_PMODE;
	value &= ~(PADCFG0_TXDIS | PADCFG0_RXDIS | PADCFG0_TXSTATE);
	if (high)
		value |= PADCFG0_TXSTATE;
	writel(value, cfg);
	readl(cfg);
}

static int pad_value(unsigned int pin)
{
	return !!(readl(padcfg(pin)) & PADCFG0_RXSTATE);
}

static void mdio_delay(void)
{
	udelay(3);
}

static void mdc(int high)
{
	pad_output(PIN_MDC, high);
	mdio_delay();
}

static void mdio_drive(int high)
{
	if (high)
		pad_input(PIN_MDIO);
	else
		pad_output(PIN_MDIO, 0);
	mdio_delay();
}

static void mdio_bit(int bit)
{
	mdc(0);
	mdio_drive(bit);
	mdc(1);
}

static int mdio_sample(void)
{
	int bit;

	mdc(0);
	mdio_drive(1);
	mdc(1);
	bit = pad_value(PIN_MDIO);
	return bit;
}

static void mdio_header(unsigned int phy, unsigned int reg, bool write)
{
	int i;

	for (i = 0; i < 32; i++)
		mdio_bit(1);
	mdio_bit(0); mdio_bit(1); /* ST */
	if (write) {
		mdio_bit(0); mdio_bit(1); /* OP write */
	} else {
		mdio_bit(1); mdio_bit(0); /* OP read */
	}
	for (i = 4; i >= 0; i--)
		mdio_bit((phy >> i) & 1);
	for (i = 4; i >= 0; i--)
		mdio_bit((reg >> i) & 1);
}

static int smi_read(unsigned int phy, unsigned int reg, u16 *value)
{
	u16 raw = 0;
	int i, ack;

	mdio_header(phy, reg, false);
	(void)mdio_sample();
	ack = !mdio_sample();
	for (i = 15; i >= 0; i--)
		raw |= mdio_sample() << i;
	mdc(0);
	mdio_drive(1);
	if (!ack)
		return -EIO;

	/* This board's SMI data phase is observed one clock early; discard the
	 * released-bus tail bit. 0x3203 consequently decodes to ID 0x1901.
	 */
	*value = raw >> 1;
	return 0;
}

static void smi_write(unsigned int phy, unsigned int reg, u16 value)
{
	int i;

	mdio_header(phy, reg, true);
	mdio_bit(1); mdio_bit(0); /* TA */
	for (i = 15; i >= 0; i--)
		mdio_bit((value >> i) & 1);
	mdc(0);
	mdio_drive(1);
}

static int port_update(int port, int reg, u16 clear, u16 set)
{
	u16 old, verify;
	int err;

	err = smi_read(port, reg, &old);
	if (err)
		return err;
	smi_write(port, reg, (old & ~clear) | set);
	err = smi_read(port, reg, &verify);
	if (err)
		return err;
	pr_info(DRV ": p%d r%02x 0x%04x -> 0x%04x\n",
		port, reg, old, verify);
	return 0;
}

static int g2_wait(void)
{
	u16 value;
	int i, err;

	for (i = 0; i < 100; i++) {
		err = smi_read(GLOBAL2, G2_SMI_CMD, &value);
		if (err)
			return err;
		if (!(value & G2_BUSY))
			return 0;
		udelay(100);
	}
	return -ETIMEDOUT;
}

static int serdes_c45_addr(u16 reg)
{
	int err = g2_wait();

	if (err)
		return err;
	smi_write(GLOBAL2, G2_SMI_DATA, reg);
	smi_write(GLOBAL2, G2_SMI_CMD,
		  G2_BUSY | (cpu_port << 5) | MDIO_MMD_PHYXS);
	return g2_wait();
}

static int serdes_read(u16 reg, u16 *value)
{
	int err = serdes_c45_addr(reg);

	if (err)
		return err;
	smi_write(GLOBAL2, G2_SMI_CMD,
		  G2_BUSY | G2_C45_READ_DATA |
		  (cpu_port << 5) | MDIO_MMD_PHYXS);
	err = g2_wait();
	if (err)
		return err;
	return smi_read(GLOBAL2, G2_SMI_DATA, value);
}

static int serdes_write(u16 reg, u16 value)
{
	int err = serdes_c45_addr(reg);

	if (err)
		return err;
	smi_write(GLOBAL2, G2_SMI_DATA, value);
	smi_write(GLOBAL2, G2_SMI_CMD,
		  G2_BUSY | G2_C45_WRITE_DATA |
		  (cpu_port << 5) | MDIO_MMD_PHYXS);
	return g2_wait();
}

static int __init edge610_switch_init(void)
{
	struct pci_dev *p2sb;
	struct pci_dev *x553;
	void __iomem *x553_bar;
	resource_size_t p2sb_bar;
	u32 padbar, nwsel, new_nwsel;
	u16 id, status;
	int port, err;

	p2sb = pci_get_device(PCI_VENDOR_ID_INTEL, P2SB_DEVICE_ID, NULL);
	if (!p2sb)
		return -ENODEV;
	p2sb_bar = pci_resource_start(p2sb, 0);
	pci_dev_put(p2sb);
	if (!p2sb_bar)
		return -ENODEV;

	north = ioremap(p2sb_bar + NORTH_OFFSET, NORTH_SIZE);
	if (!north)
		return -ENOMEM;
	padbar = readl(north + 0x0c);
	if (readl(north) == 0xffffffff || padbar >= NORTH_SIZE) {
		err = -ENODEV;
		goto fail_unmap;
	}
	pads = north + padbar;
	if (cpu_port != 9 && cpu_port != 10) {
		err = -EINVAL;
		goto fail_unmap;
	}

	pad_output(PIN_MDC, 0);
	pad_input(PIN_MDIO);
	pad_output(PIN_RESET, 0);
	msleep(100);
	pad_output(PIN_RESET, 1);
	msleep(500);

	err = smi_read(0, PORT_SWITCH_ID, &id);
	if (err || (id & 0xfff0) != 0x1900) {
		pr_err(DRV ": unexpected switch ID 0x%04x (err=%d)\n", id, err);
		err = -ENODEV;
		goto fail_unmap;
	}
	pr_info(DRV ": Marvell 88E6190 rev %u at P2SB BAR %pa\n",
		id & 0xf, &p2sb_bar);

	if (x553_sgmii) {
		x553 = pci_get_domain_bus_and_slot(0, 3,
			PCI_DEVFN(0, cpu_port == 10 ? 0 : 1));
		if (!x553) {
			err = -ENODEV;
			goto fail_unmap;
		}
		x553_bar = pci_iomap(x553, 0, 0);
		if (!x553_bar) {
			pci_dev_put(x553);
			err = -ENOMEM;
			goto fail_unmap;
		}
		nwsel = readl(x553_bar + X553_NW_MNG_IF_SEL);
		new_nwsel = (nwsel & ~GENMASK(21, 17)) | BIT(19) | BIT(25);
		writel(new_nwsel, x553_bar + X553_NW_MNG_IF_SEL);
		readl(x553_bar + X553_NW_MNG_IF_SEL);
		pci_iounmap(x553, x553_bar);
		pci_dev_put(x553);
		pr_info(DRV ": X553 NW_MNG_IF_SEL 0x%08x -> 0x%08x\n",
			nwsel, new_nwsel);
		port_update(cpu_port, 0, 0x000f, 0x000a); /* SGMII C_Mode */
	}

	/* Isolate each front-panel port to the primary CPU port (10). */
	for (port = 1; port <= 6; port++) {
		err = port_update(port, PORT_BASE_VLAN, 0x07ff, BIT(cpu_port));
		if (err)
			goto fail_unmap;
		port_update(port, PORT_DEFAULT_VLAN, 0x0fff, 1);
		port_update(port, PORT_CTL2, PORT_8021Q_MODE_MASK, 0);
		port_update(port, PORT_CTL0,
			    PORT_FRAME_MODE_MASK | PORT_STATE_MASK,
			    PORT_STATE_FORWARDING);
	}

	/* CPU port: normal untagged frames, 2.5 Gbit full duplex, link forced up.
	 * The X553 function advertises only 2500baseT/Full on this board. For the
	 * 6390 family, 2.5G is ForceSpeed + AltSpeed + speed value 3.
	 */
	port_update(cpu_port, PORT_BASE_VLAN, 0x07ff, GENMASK(6, 1));
	port_update(cpu_port, PORT_DEFAULT_VLAN, 0x0fff, 1);
	port_update(cpu_port, PORT_CTL2, PORT_8021Q_MODE_MASK, 0);
	port_update(cpu_port, PORT_CTL0,
		    PORT_FRAME_MODE_MASK | PORT_STATE_MASK,
		    PORT_STATE_FORWARDING);
	port_update(cpu_port, PORT_MAC_CTL, 0xffff, cpu_mac_ctl);

	/* Power the 2.5GBASE-X PCS and advertise full duplex with symmetric pause.
	 * Enabling AN triggers negotiation with the X553 link partner.
	 */
	if (!serdes_read(SERDES_BMCR, &status)) {
		pr_info(DRV ": serdes BMCR before=0x%04x\n", status);
		status &= ~(BIT(15) | BIT(14) | BIT(11)); /* reset/loopback/pdown */
		if (serdes_autoneg)
			status |= BIT(12) | BIT(9); /* AN enable + restart */
		else
			status &= ~(BIT(12) | BIT(9));
		serdes_write(SERDES_ADVERTISE, 0x00e0);
		serdes_write(SERDES_BMCR, status);
		msleep(100);
		if (!serdes_read(SERDES_BMCR, &status))
			pr_info(DRV ": serdes BMCR after=0x%04x\n", status);
		if (!serdes_read(SERDES_BMSR, &status))
			pr_info(DRV ": serdes BMSR=0x%04x\n", status);
		if (!serdes_read(SERDES_LPA, &status))
			pr_info(DRV ": serdes LPA=0x%04x\n", status);
		if (!serdes_read(SERDES_PHY_STATUS, &status))
			pr_info(DRV ": serdes PHY status=0x%04x\n", status);
	}

	for (port = 1; port <= 6; port++) {
		if (!smi_read(port, 0, &status))
			pr_info(DRV ": ge%d status=0x%04x link=%s\n", port,
				status, status & BIT(11) ? "up" : "down");
	}
	if (!smi_read(cpu_port, 0, &status))
		pr_info(DRV ": cpu%u status=0x%04x link=%s\n", cpu_port, status,
			status & BIT(11) ? "up" : "down");

	return 0;

fail_unmap:
	iounmap(north);
	north = NULL;
	return err;
}

static void __exit edge610_switch_exit(void)
{
	int port;

	for (port = 1; port <= 6; port++)
		port_update(port, PORT_CTL0, PORT_STATE_MASK, 0);
	port_update(cpu_port, PORT_CTL0, PORT_STATE_MASK, 0);
	if (north)
		iounmap(north);
	pr_info(DRV ": front-panel and CPU ports disabled\n");
}

module_init(edge610_switch_init);
module_exit(edge610_switch_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("VMware Edge 610 Marvell 88E6190 standalone switch setup");
