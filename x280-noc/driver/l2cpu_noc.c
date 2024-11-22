/* SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Tenstorrent L2CPU NOC Window Management Driver Interface
 *
 * The L2CPU is a hardware block in the Tenstorrent Blackhole chip containing
 * four RISC-V cores and a Network-on-Chip (NOC) interface.  This driver runs on
 * the RISC-V cores and manages access to the rest of the chip through
 * configurable NOC windows.
 *
 * Hardware provides two window sizes:
 *
 * 2M (2 MiB; 0x200000 bytes) - 224 windows
 * 128G (128 GiB; 0x2000000000 bytes) - 32 windows
 *
 * Some windows may be reserved by firmware.  A dedicated window is reserved for
 * driver telemetry access.
 */

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#include "ioctl.h"

#define DRIVER_NAME "l2cpu-noc"

// System port is a 64 TiB region that is not cached.
// All NOC windows are mapped within this region.
#define SYSTEM_PORT 0x30000000ULL

// Sizes, addresses, quantities of the NOC windows.
#define WINDOW_2M_COUNT 224
#define WINDOW_2M_SHIFT 21
#define WINDOW_2M_SIZE (1 << WINDOW_2M_SHIFT)
#define WINDOW_2M_BASE (((1UL << 37) | SYSTEM_PORT))
#define WINDOW_2M_ADDR(n) (WINDOW_2M_BASE + (WINDOW_2M_SIZE * n))
#define WINDOW_128G_COUNT 32
#define WINDOW_128G_SHIFT 37
#define WINDOW_128G_SIZE (1UL << WINDOW_128G_SHIFT)
//#define WINDOW_128G_BASE (((1UL << 43) | (1UL << 37) | SYSTEM_PORT))
#define WINDOW_128G_BASE 0x480430000000ULL
#define WINDOW_128G_ADDR(n) (WINDOW_128G_BASE + (WINDOW_128G_SIZE * (n)))

// TLB registers control NOC window configuration.
#define TLB_2M_CONFIG_BASE 0x2ff00000
#define TLB_2M_CONFIG_SIZE (0x10 * WINDOW_2M_COUNT)
#define TLB_128G_CONFIG_BASE (TLB_2M_CONFIG_BASE + TLB_2M_CONFIG_SIZE)
#define TLB_128G_CONFIG_SIZE (0x0c * WINDOW_128G_COUNT)

// ARC owns telemetry
#define ARC_X 8
#define ARC_Y 0
#define ARC_TELEMETRY_PTR 0x80030434

/**
 * struct l2cpu_noc_dev - NOC device state
 * @lock: Guards bitmaps
 * @reserved_2M: Bitmap of reserved 2M windows
 * @reserved_128G: Bitmap of reserved 128G windows
 * @allocated_2M: Bitmap of allocated 2M windows
 * @allocated_128G: Bitmap of allocated 128G windows
 * @config_regs_2M: 2M TLB registers
 * @config_regs_128G: 128G TLB registers
 * @driver_window_id: ID of driver telemetry window
 * @driver_window: Driver telemetry window
 */
struct l2cpu_noc_dev {
	struct platform_device *pdev;
	struct class *dev_class;
	struct device *dev;
	dev_t dev_num;
	struct cdev cdev;
	struct device *hwmon_dev;
	struct dentry *debugfs_dir;

	struct mutex lock;
	DECLARE_BITMAP(reserved_2M, WINDOW_2M_COUNT);
	DECLARE_BITMAP(reserved_128G, WINDOW_128G_COUNT);
	DECLARE_BITMAP(allocated_2M, WINDOW_2M_COUNT);
	DECLARE_BITMAP(allocated_128G, WINDOW_128G_COUNT);

	void __iomem *config_regs_2M;
	void __iomem *config_regs_128G;

	int driver_window_id;
	void __iomem *driver_window;
};

/**
 * struct l2cpu_noc_fd - Per file descriptor state
 * @noc: NOC device
 * @owned_2M: Bitmap of 2M windows owned by this file descriptor
 * @owned_128G: Bitmap of 128G windows owned by this file descriptor
 */
struct l2cpu_noc_fd {
	struct l2cpu_noc_dev *noc;
	DECLARE_BITMAP(owned_2M, WINDOW_2M_COUNT);
	DECLARE_BITMAP(owned_128G, WINDOW_128G_COUNT);
};

/**
 * struct TLB_2M_REG - TLB register layout for 2M windows
 */
struct TLB_2M_REG {
	union {
		struct {
			u32 data[4];
		};
		struct {
			u64 addr : 43;
			u64 reserved0 : 21;
			u64 x_end : 6;
			u64 y_end : 6;
			u64 x_start : 6;
			u64 y_start : 6;
			u64 multicast_en : 1;
			u64 strict_order : 1;
			u64 posted : 1;
			u64 linked : 1;
			u64 static_en : 1;
			u64 stream_header : 1;
			u64 reserved1 : 1;
			u64 noc_selector : 1;
			u64 static_vc : 3;
			u64 strided : 8;
			u64 exclude_coord_x : 5;
			u64 exclude_coord_y : 4;
			u64 exclude_dir_x : 1;
			u64 exclude_dir_y : 1;
			u64 exclude_enable : 1;
			u64 exclude_routing_option : 1;
			u64 num_destinations : 8;
		};
	};
};

/**
 * struct TLB_128G_REG - TLB register layout for 128G windows
 */
struct TLB_128G_REG {
	union {
		struct {
			u32 data[3];
		};
		struct {
			u64 addr : 27;
			u64 reserved0 : 5;
			u64 x_end : 6;
			u64 y_end : 6;
			u64 x_start : 6;
			u64 y_start : 6;
			u64 multicast_en : 1;
			u64 strict_order : 1;
			u64 posted : 1;
			u64 linked : 1;
			u64 static_en : 1;
			u64 stream_header : 1;
			u64 reserved1 : 1;
			u64 noc_selector : 1;
			u64 static_vc : 3;
			u64 strided : 8;
			u64 exclude_coord_x : 5;
			u64 exclude_coord_y : 4;
			u64 exclude_dir_x : 1;
			u64 exclude_dir_y : 1;
			u64 exclude_enable : 1;
			u64 exclude_routing_option : 1;
			u64 num_destinations : 8;
		};
	};
};

/**
 * l2cpu_noc_stats_show - Show window utilization statistics
 *
 * Part of the debugfs interface.
 */
static int l2cpu_noc_stats_show(struct seq_file *m, void *unused)
{
	struct l2cpu_noc_dev *noc = m->private;
	unsigned int allocated_2M = 0;
	unsigned int allocated_128G = 0;
	unsigned int reserved_2M = 0;
	unsigned int reserved_128G = 0;

	mutex_lock(&noc->lock);
	allocated_2M = bitmap_weight(noc->allocated_2M, WINDOW_2M_COUNT);
	allocated_128G = bitmap_weight(noc->allocated_128G, WINDOW_128G_COUNT);
	reserved_2M = bitmap_weight(noc->reserved_2M, WINDOW_2M_COUNT);
	reserved_128G = bitmap_weight(noc->reserved_128G, WINDOW_128G_COUNT);
	mutex_unlock(&noc->lock);

	seq_printf(m, "2M windows allocated: %u\n", allocated_2M);
	seq_printf(m, "2M windows reserved: %u\n", reserved_2M);
	seq_printf(m, "2M windows free: %u\n", WINDOW_2M_COUNT - allocated_2M - reserved_2M);
	seq_printf(m, "128G windows allocated: %u\n", allocated_128G);
	seq_printf(m, "128G windows reserved: %u\n", reserved_128G);
	seq_printf(m, "128G windows free: %u\n", WINDOW_128G_COUNT - allocated_128G - reserved_128G);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(l2cpu_noc_stats);

/**
 * configure_window_2M - Configure a 2M window
 * Return: 0 on success, -EINVAL on error
 */
static int configure_window_2M(struct l2cpu_noc_dev *noc, struct noc_window_config *config)
{
	struct TLB_2M_REG reg = { 0 };
	int window_id = config->window_id;
	u32 offset;

	if (window_id < 0 || window_id >= WINDOW_2M_COUNT)
		return -EINVAL;

	if (config->addr & (WINDOW_2M_SIZE - 1))
		return -EINVAL;

	reg.addr = config->addr >> WINDOW_2M_SHIFT;
	reg.x_end = config->x_end;
	reg.y_end = config->y_end;
	reg.x_start = config->x_start;
	reg.y_start = config->y_start;
	reg.multicast_en = config->multicast_en;
	reg.strict_order = config->strict_order;
	reg.posted = config->posted;
	reg.linked = config->linked;
	reg.static_en = config->static_en;
	reg.stream_header = config->stream_header;
	reg.noc_selector = config->noc_selector;
	reg.static_vc = config->static_vc;

	offset = window_id * 0x10;

	mb();
	writel(reg.data[0], noc->config_regs_2M + offset + 0x0);
	writel(reg.data[1], noc->config_regs_2M + offset + 0x4);
	writel(reg.data[2], noc->config_regs_2M + offset + 0x8);
	writel(reg.data[3], noc->config_regs_2M + offset + 0xC);
	wmb();

	return 0;
}

/**
 * configure_window_128G - Configure a 128G window
 * Return: 0 on success, -EINVAL on error
 */
static int configure_window_128G(struct l2cpu_noc_dev *noc, struct noc_window_config *config)
{
	struct TLB_128G_REG reg = { 0 };
	int window_id = config->window_id;
	u32 offset;

	if (window_id < 0 || window_id >= WINDOW_128G_COUNT)
		return -EINVAL;

	if (config->addr & (WINDOW_128G_SIZE - 1))
		return -EINVAL;

	reg.addr = config->addr >> WINDOW_128G_SHIFT;
	reg.x_end = config->x_end;
	reg.y_end = config->y_end;
	reg.x_start = config->x_start;
	reg.y_start = config->y_start;
	reg.multicast_en = config->multicast_en;
	reg.strict_order = config->strict_order;
	reg.posted = config->posted;
	reg.linked = config->linked;
	reg.static_en = config->static_en;
	reg.stream_header = config->stream_header;
	reg.noc_selector = config->noc_selector;
	reg.static_vc = config->static_vc;

	offset = window_id * 0xC;

	mb();
	writel(reg.data[0], noc->config_regs_128G + offset + 0x0);
	writel(reg.data[1], noc->config_regs_128G + offset + 0x4);
	writel(reg.data[2], noc->config_regs_128G + offset + 0x8);
	wmb();

	return 0;
}

/**
 * is_2M_window_configured - Check if a 2M window is configured
 * Return: true if configured, false otherwise
 */
static bool is_2M_window_configured(struct l2cpu_noc_dev *noc, int window_id)
{
	struct TLB_2M_REG reg = { 0 };
	u32 offset = window_id * 0x10;

	if (window_id < 0 || window_id >= WINDOW_2M_COUNT)
		return false;

	reg.data[0] = readl(noc->config_regs_2M + offset + 0x0);
	reg.data[1] = readl(noc->config_regs_2M + offset + 0x4);
	reg.data[2] = readl(noc->config_regs_2M + offset + 0x8);
	reg.data[3] = readl(noc->config_regs_2M + offset + 0xC);

	return reg.addr != window_id || reg.data[1] || reg.data[2] || reg.data[3];
}

/**
 * is_128G_window_configured - Check if a 128G window is configured
 * Return: true if configured, false otherwise
 */
static bool is_128G_window_configured(struct l2cpu_noc_dev *noc, int window_id)
{
	struct TLB_128G_REG reg = { 0 };
	u32 offset = window_id * 0xc;

	if (window_id < 0 || window_id >= WINDOW_128G_COUNT)
		return false;

	reg.data[0] = readl(noc->config_regs_128G + offset + 0x0);
	reg.data[1] = readl(noc->config_regs_128G + offset + 0x4);
	reg.data[2] = readl(noc->config_regs_128G + offset + 0x8);

	return reg.addr != window_id || reg.data[1] || reg.data[2];
}

/**
 * scan_configured_windows - Find pre-configured windows
 *
 * Scans TLB registers to find windows that were configured by firmware or other
 * systems software.  These windows are marked as reserved to prevent the driver
 * from allocating them.
 */
static void scan_configured_windows(struct l2cpu_noc_dev *noc)
{
	int window_id;

	for (window_id = 0; window_id < WINDOW_2M_COUNT; window_id++) {
		if (is_2M_window_configured(noc, window_id))
			set_bit(window_id, noc->reserved_2M);
	}

	for (window_id = 0; window_id < WINDOW_128G_COUNT; window_id++) {
		if (is_128G_window_configured(noc, window_id))
			set_bit(window_id, noc->reserved_128G);
	}
}

/**
 * allocate_2M_window - Allocate a 2M window
 * Returns: Window ID on success, -ENOSPC if no windows are available
 */
static int allocate_2M_window(struct l2cpu_noc_dev *noc)
{
	int window_id;
	DECLARE_BITMAP(in_use, WINDOW_2M_COUNT);

	bitmap_or(in_use, noc->reserved_2M, noc->allocated_2M, WINDOW_2M_COUNT);

	window_id = find_first_zero_bit(in_use, WINDOW_2M_COUNT);
	if (window_id >= WINDOW_2M_COUNT) {
		return -ENOSPC;
	}

	set_bit(window_id, noc->allocated_2M);

	return window_id;
}

/**
 * allocate_128G_window - Allocate a 128G window
 * Returns: Window ID on success, -ENOSPC if no windows are available
 */
static int allocate_128G_window(struct l2cpu_noc_dev *noc)
{
	int window_id;
	DECLARE_BITMAP(in_use, WINDOW_128G_COUNT);

	bitmap_or(in_use, noc->reserved_128G, noc->allocated_128G, WINDOW_128G_COUNT);

	window_id = find_first_zero_bit(in_use, WINDOW_128G_COUNT);
	if (window_id >= WINDOW_128G_COUNT) {
		return -ENOSPC;
	}

	set_bit(window_id, noc->allocated_128G);

	return window_id;
}

/**
 * deallocate_2M_window - Deallocate a 2M window
 * Return: 0 on success, -EINVAL if ID is invalid, -EBUSY if window is in use
 */
static int deallocate_2M_window(struct l2cpu_noc_dev *noc, int window_id)
{
	struct noc_window_config config = { 0 };

	if (window_id < 0 || window_id >= WINDOW_2M_COUNT)
		return -EINVAL;

	clear_bit(window_id, noc->allocated_2M);

	config.window_id = window_id;
	config.addr = window_id << WINDOW_2M_SHIFT;
	configure_window_2M(noc, &config);

	return 0;
}

/**
 * deallocate_128G_window - Deallocate a 128G window
 * Return: 0 on success, -EINVAL if ID is invalid, -EBUSY if window is in use
 */
static int deallocate_128G_window(struct l2cpu_noc_dev *noc, int window_id)
{
	struct noc_window_config config = { 0 };

	if (window_id < 0 || window_id >= WINDOW_128G_COUNT)
		return -EINVAL;

	clear_bit(window_id, noc->allocated_128G);

	config.window_id = window_id;
	config.addr = (u64)window_id << WINDOW_128G_SHIFT;
	configure_window_128G(noc, &config);

	return 0;
}

/**
 * l2cpu_cdev_open - Open file descriptor
 */
static int l2cpu_cdev_open(struct inode *inode, struct file *file)
{
	struct l2cpu_noc_fd *fd = kzalloc(sizeof(*fd), GFP_KERNEL);

	if (!fd)
		return -ENOMEM;

	fd->noc = container_of(inode->i_cdev, struct l2cpu_noc_dev, cdev);
	file->private_data = fd;

	return 0;
}

/**
 * l2cpu_cdev_release - Release file descriptor
 *
 * Deallocates all windows owned by this file descriptor.
 */
static int l2cpu_cdev_release(struct inode *inode, struct file *file)
{
	struct l2cpu_noc_fd *fd = file->private_data;
	struct l2cpu_noc_dev *noc = fd->noc;
	int window_id;

	mutex_lock(&noc->lock);

	for_each_set_bit(window_id, fd->owned_2M, WINDOW_2M_COUNT)
		deallocate_2M_window(noc, window_id);

	for_each_set_bit(window_id, fd->owned_128G, WINDOW_128G_COUNT)
		deallocate_128G_window(noc, window_id);

	mutex_unlock(&noc->lock);

	kfree(fd);
	return 0;
}

/**
 * ioctl_alloc_2M - Allocate a 2M window
 */
static long ioctl_alloc_2M(struct l2cpu_noc_fd *fd, unsigned long arg)
{
	struct l2cpu_noc_dev *noc = fd->noc;
	struct noc_window_handle handle = { 0 };
	int window_id = allocate_2M_window(noc);

	if (window_id < 0)
		return window_id;

	handle.window_id = window_id;
	handle.mmap_offset = WINDOW_2M_ADDR(window_id);
	handle.mmap_size = 1UL << WINDOW_2M_SHIFT;

	if (copy_to_user((void __user *)arg, &handle, sizeof(handle))) {
		deallocate_2M_window(noc, window_id);
		return -EFAULT;
	}

	set_bit(window_id, fd->owned_2M);

	return 0;
}

/**
 * ioctl_alloc_128G - Allocate a 128G window
 */
static long ioctl_alloc_128G(struct l2cpu_noc_fd *fd, unsigned long arg)
{
	struct l2cpu_noc_dev *noc = fd->noc;
	struct noc_window_handle handle = { 0 };
	int window_id = allocate_128G_window(noc);

	if (window_id < 0)
		return window_id;

	handle.window_id = window_id;
	handle.mmap_offset = WINDOW_128G_ADDR(window_id);
	handle.mmap_size = 1UL << WINDOW_128G_SHIFT;

	if (copy_to_user((void __user *)arg, &handle, sizeof(handle))) {
		deallocate_128G_window(noc, window_id);
		return -EFAULT;
	}

	set_bit(window_id, fd->owned_128G);

	return 0;
}

/**
 * ioctl_config_2M - Configure a 2M window
 */
static long ioctl_config_2M(struct l2cpu_noc_fd *fd, unsigned long arg)
{
	struct noc_window_config config = { 0 };

	if (copy_from_user(&config, (void __user *)arg, sizeof(config)))
		return -EFAULT;

	if (!test_bit(config.window_id, fd->owned_2M))
		return -EPERM;

	return configure_window_2M(fd->noc, &config);
}

/**
 * ioctl_config_128G - Configure a 128G window
 */
static long ioctl_config_128G(struct l2cpu_noc_fd *fd, unsigned long arg)
{
	struct noc_window_config config = { 0 };

	if (copy_from_user(&config, (void __user *)arg, sizeof(config)))
		return -EFAULT;

	if (!test_bit(config.window_id, fd->owned_128G))
		return -EPERM;

	return configure_window_128G(fd->noc, &config);
}

/**
 * ioctl_dealloc_2M - Deallocate a 2M window
 */
static long ioctl_dealloc_2M(struct l2cpu_noc_fd *fd, unsigned long arg)
{
	struct l2cpu_noc_dev *noc = fd->noc;
	struct noc_window_handle handle = { 0 };

	if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
		return -EFAULT;

	if (!test_bit(handle.window_id, fd->owned_2M))
		return -EPERM;

	clear_bit(handle.window_id, fd->owned_2M);

	return deallocate_2M_window(noc, handle.window_id);
}

/**
 * ioctl_dealloc_128G - Deallocate a 128G window
 */
static long ioctl_dealloc_128G(struct l2cpu_noc_fd *fd, unsigned long arg)
{
	struct l2cpu_noc_dev *noc = fd->noc;
	struct noc_window_handle handle = { 0 };

	if (copy_from_user(&handle, (void __user *)arg, sizeof(handle)))
		return -EFAULT;

	if (!test_bit(handle.window_id, fd->owned_128G))
		return -EPERM;

	clear_bit(handle.window_id, fd->owned_128G);

	return deallocate_128G_window(noc, handle.window_id);
}

/**
 * l2cpu_cdev_ioctl - Handle ioctl commands
 */
static long l2cpu_cdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct l2cpu_noc_fd *fd = file->private_data;
	struct l2cpu_noc_dev *noc;
	int ret;

	if (!fd)
		return -EINVAL;

	noc = fd->noc;

	if (_IOC_DIR(cmd) & (_IOC_READ | _IOC_WRITE)) {
		if (!arg)
			return -EINVAL;

		if (!access_ok((void __user *)arg, _IOC_SIZE(cmd)))
			return -EFAULT;
	}

	mutex_lock(&noc->lock);

	switch (cmd) {
	case L2CPU_IOCTL_ALLOC_2M:
		ret = ioctl_alloc_2M(fd, arg);
		break;

	case L2CPU_IOCTL_ALLOC_128G:
		ret = ioctl_alloc_128G(fd, arg);
		break;

	case L2CPU_IOCTL_CONFIG_2M:
		ret = ioctl_config_2M(fd, arg);
		break;

	case L2CPU_IOCTL_CONFIG_128G:
		ret = ioctl_config_128G(fd, arg);
		break;

	case L2CPU_IOCTL_DEALLOC_2M:
		ret = ioctl_dealloc_2M(fd, arg);
		break;

	case L2CPU_IOCTL_DEALLOC_128G:
		ret = ioctl_dealloc_128G(fd, arg);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&noc->lock);

	return ret;
}

/**
 * l2cpu_cdev_mmap - Memory map a NOC window
 */
static int l2cpu_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct l2cpu_noc_fd *fd = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	bool is_2M = offset < WINDOW_128G_BASE;
	unsigned long max_size = is_2M ? WINDOW_2M_SIZE : WINDOW_128G_SIZE;
	int window_id;
	int ret = 0;

	if (size > max_size)
		return -EINVAL;

	mutex_lock(&fd->noc->lock);

	if (is_2M) {
		window_id = (offset - WINDOW_2M_BASE) >> WINDOW_2M_SHIFT;
		if (window_id < 0 || window_id >= WINDOW_2M_COUNT) {
			ret = -EINVAL;
			goto out_unlock;
		}

		if (!test_bit(window_id, fd->owned_2M)) {
			ret = -EPERM;
			goto out_unlock;
		}
	} else {
		window_id = (offset - WINDOW_128G_BASE) >> WINDOW_128G_SHIFT;
		if (window_id < 0 || window_id >= WINDOW_128G_COUNT) {
			ret = -EINVAL;
			goto out_unlock;
		}
		if (!test_bit(window_id, fd->owned_128G)) {
			ret = -EPERM;
			goto out_unlock;
		}
	}

	// TODO: is there a way to use hugepages for IO memory here?
	// I don't want to eat up the system's DRAM for hugepages, just use fewer
	// CPU TLB entries for the NOC windows.

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	ret = io_remap_pfn_range(vma, vma->vm_start, offset >> PAGE_SHIFT, size, vma->vm_page_prot);
	if (ret)
		ret = -EAGAIN;

out_unlock:
	mutex_unlock(&fd->noc->lock);
	return ret;
}

static const struct file_operations chardev_fops = {
	.owner = THIS_MODULE,
	.open = l2cpu_cdev_open,
	.release = l2cpu_cdev_release,
	.unlocked_ioctl = l2cpu_cdev_ioctl,
	.mmap = l2cpu_cdev_mmap,
};

/**
 * noc_read32 - Read a 32-bit value from the NOC
 *
 * Configures the driver's 2M window to read from the specified address.
 */
static u32 noc_read32(struct l2cpu_noc_dev *noc, u32 x, u32 y, u64 addr)
{
	struct noc_window_config config = { 0 };
	u32 val;

	config.window_id = noc->driver_window_id;
	config.addr = addr & ~(WINDOW_2M_SIZE - 1);
	config.x_end = x;
	config.y_end = y;

	configure_window_2M(noc, &config);
	val = readl(noc->driver_window + (addr & (WINDOW_2M_SIZE - 1)));

	return val;
}

/**
 * initialize_driver_window - Initialize the driver telemetry window
 *
 * Finds an available 2M window and maps it for the driver's use.
 */
static int initialize_driver_window(struct l2cpu_noc_dev *noc)
{
	void *window;
	int window_id;

	window_id = find_first_zero_bit(noc->reserved_2M, WINDOW_2M_COUNT);
	if (window_id >= WINDOW_2M_COUNT) {
		pr_err("No available 2M windows\n");
		return -ENOSPC;
	}

	window = devm_ioremap(&noc->pdev->dev, WINDOW_2M_ADDR(window_id), WINDOW_2M_SIZE);
	if (!window)
		return -ENOMEM;

	noc->driver_window_id = window_id;
	noc->driver_window = window;

	set_bit(window_id, noc->allocated_2M);

	return 0;
}

struct l2cpu_hwmon_label {
	enum hwmon_sensor_types type;
	u32 attr;
	const char *label;
};

struct l2cpu_hwmon_attr {
	u32 syseng_magic;
	enum hwmon_sensor_types type;
	u32 attr;
	u32 addr;
};

static const struct l2cpu_hwmon_label l2cpu_hwmon_labels[] = {
	{ hwmon_temp, hwmon_temp_label, "ASIC Temperature" },
	{ hwmon_in, hwmon_in_label, "VCORE" },
};

static struct l2cpu_hwmon_attr l2cpu_hwmon_attrs[] = {
	{ 11, hwmon_temp, hwmon_temp_input, 0x0 },
	{ 6, hwmon_in, hwmon_in_input, 0x0 },
};

/**
 * l2cpu_hwmon_is_visible - Check if a sensor is visible
 *
 * Part of the hwmon_ops interface.
 */
static umode_t l2cpu_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(l2cpu_hwmon_labels); i++) {
		if (type == l2cpu_hwmon_labels[i].type && attr == l2cpu_hwmon_labels[i].attr)
			return S_IRUGO;
	}

	for (i = 0; i < ARRAY_SIZE(l2cpu_hwmon_attrs); i++) {
		if (type == l2cpu_hwmon_attrs[i].type && attr == l2cpu_hwmon_attrs[i].attr)
			return S_IRUGO;
	}

	return 0;
}

/**
 * l2cpu_hwmon_read - Read a sensor value
 *
 * Part of the hwmon_ops interface.
 */
static int l2cpu_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	struct l2cpu_noc_dev *noc = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(l2cpu_hwmon_attrs); i++) {
		if (type == l2cpu_hwmon_attrs[i].type && attr == l2cpu_hwmon_attrs[i].attr) {
			u32 raw = noc_read32(noc, ARC_X, ARC_Y, l2cpu_hwmon_attrs[i].addr);

			if (type == hwmon_temp) {
				u32 int_part = raw >> 16;
				u32 frac_part = raw & 0xffff;
				*val = (int_part * 1000) + ((frac_part * 1000) / 65536);
			} else {
				*val = raw;
			}
			return 0;
		}
	}

	return -EOPNOTSUPP;
}

/**
 * l2cpu_hwmon_read_string - Read a sensor label
 *
 * Part of the hwmon_ops interface.
 */
static int l2cpu_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
				   const char **str)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(l2cpu_hwmon_labels); i++) {
		if (type == l2cpu_hwmon_labels[i].type && attr == l2cpu_hwmon_labels[i].attr) {
			*str = l2cpu_hwmon_labels[i].label;
			return 0;
		}
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info *l2cpu_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL),
	NULL,
};

static const struct hwmon_ops l2cpu_hwmon_ops = {
	.is_visible = l2cpu_hwmon_is_visible,
	.read = l2cpu_hwmon_read,
	.read_string = l2cpu_hwmon_read_string,
};

static const struct hwmon_chip_info l2cpu_hwmon_chip_info = {
	.ops = &l2cpu_hwmon_ops,
	.info = l2cpu_hwmon_channel_info,
};

/**
 * initialize_hwmon - Initialize the hwmon interface
 *
 * Discovers addresses for telemetry data and registers the hwmon device.
 */
static int initialize_hwmon(struct l2cpu_noc_dev *noc)
{
	struct platform_device *pdev = noc->pdev;
	struct device *hwmon_device;
	u32 base_addr, num_entries;
	u32 tags_addr, data_addr;
	u32 tag_entry, offset;
	int i, j;

	// Do the stupid discovery dance
	base_addr = noc_read32(noc, ARC_X, ARC_Y, ARC_TELEMETRY_PTR);
	num_entries = noc_read32(noc, ARC_X, ARC_Y, base_addr + 0x4);

	if (num_entries > 1024)
		return -EINVAL;

	tags_addr = base_addr + 0x8;
	data_addr = tags_addr + (num_entries * 4);

	for (i = 0; i < ARRAY_SIZE(l2cpu_hwmon_attrs); ++i) {
		struct l2cpu_hwmon_attr *hwmon_attr = &l2cpu_hwmon_attrs[i];
		// Scan the tag array for our tag
		for (j = 0; j < num_entries; ++j) {
			tag_entry = noc_read32(noc, ARC_X, ARC_Y, tags_addr + (j * 4));
			if ((tag_entry & 0xff) == hwmon_attr->syseng_magic) {
				// Found it - update the address in our hwmon attribute
				offset = (tag_entry >> 16) & 0xff;
				hwmon_attr->addr = data_addr + (offset * 4);
				break;
			}
		}
	}

	hwmon_device = devm_hwmon_device_register_with_info(&pdev->dev, "blackhole", noc, &l2cpu_hwmon_chip_info, NULL);

	if (IS_ERR(hwmon_device))
		return PTR_ERR(hwmon_device);

	return 0;
}

/**
 * l2cpu_platform_probe - Probe function for the L2CPU NOC driver
 *
 * Initializes the driver state and registers the character device.
 */
static int l2cpu_platform_probe(struct platform_device *pdev)
{
	struct l2cpu_noc_dev *noc;
	struct device *dev;
	int ret;

	noc = devm_kzalloc(&pdev->dev, sizeof(*noc), GFP_KERNEL);
	if (!noc)
		return -ENOMEM;

	mutex_init(&noc->lock);
	noc->pdev = pdev;
	noc->config_regs_2M = devm_ioremap(&pdev->dev, TLB_2M_CONFIG_BASE, TLB_2M_CONFIG_SIZE);
	if (!noc->config_regs_2M)
		return -ENOMEM;

	noc->config_regs_128G = devm_ioremap(&pdev->dev, TLB_128G_CONFIG_BASE, TLB_128G_CONFIG_SIZE);
	if (!noc->config_regs_128G)
		return -ENOMEM;

	ret = alloc_chrdev_region(&noc->dev_num, 0, 1, DRIVER_NAME);
	if (ret < 0)
		return ret;

	cdev_init(&noc->cdev, &chardev_fops);
	ret = cdev_add(&noc->cdev, noc->dev_num, 1);
	if (ret < 0)
		goto err_cdev;

	noc->dev_class = class_create(DRIVER_NAME);
	if (IS_ERR(noc->dev_class)) {
		ret = PTR_ERR(noc->dev_class);
		goto err_class;
	}

	dev = device_create(noc->dev_class, NULL, noc->dev_num, noc, DRIVER_NAME);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto err_device;
	}
	noc->dev = dev;

	platform_set_drvdata(pdev, noc);
	scan_configured_windows(noc);

	ret = initialize_driver_window(noc);
	if (ret)
		goto err_init_window;

	initialize_hwmon(noc);

	noc->debugfs_dir = debugfs_create_dir(DRIVER_NAME, NULL);
	if (!IS_ERR_OR_NULL(noc->debugfs_dir))
		debugfs_create_file("stats", 0400, noc->debugfs_dir, noc, &l2cpu_noc_stats_fops);

	dev_info(&pdev->dev, "L2CPU NOC driver initialized\n");
	return 0;

err_init_window:
	device_destroy(noc->dev_class, noc->dev_num);
err_device:
	class_destroy(noc->dev_class);
err_class:
	cdev_del(&noc->cdev);
err_cdev:
	unregister_chrdev_region(noc->dev_num, 1);
	return ret;
}

/**
 * l2cpu_platform_remove - Remove function for the L2CPU NOC driver
 *
 * Deallocates all windows and cleans up the driver state.
 */
static int l2cpu_platform_remove(struct platform_device *pdev)
{
	struct l2cpu_noc_dev *noc = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < WINDOW_2M_COUNT; i++) {
		if (test_bit(i, noc->allocated_2M))
			deallocate_2M_window(noc, i);
	}
	for (i = 0; i < WINDOW_128G_COUNT; i++) {
		if (test_bit(i, noc->allocated_128G))
			deallocate_128G_window(noc, i);
	}

	if (noc->debugfs_dir)
		debugfs_remove_recursive(noc->debugfs_dir);
	device_destroy(noc->dev_class, noc->dev_num);
	class_destroy(noc->dev_class);
	cdev_del(&noc->cdev);
	unregister_chrdev_region(noc->dev_num, 1);

	return 0;
}

static struct platform_device *l2cpu_noc_pdev;
static struct platform_driver l2cpu_noc_driver = {
	.probe = l2cpu_platform_probe,
	.remove = l2cpu_platform_remove,
	.driver = {
		.name = DRIVER_NAME,
	},
};

/**
 * l2cpu_init - Module initialization function
 */
static int __init l2cpu_init(void)
{
	int ret;

	l2cpu_noc_pdev = platform_device_alloc("l2cpu-noc", 0);
	if (!l2cpu_noc_pdev)
		return -ENOMEM;

	ret = platform_device_add(l2cpu_noc_pdev);
	if (ret) {
		platform_device_put(l2cpu_noc_pdev);
		return ret;
	}

	return platform_driver_register(&l2cpu_noc_driver);
}

/**
 * l2cpu_exit - Module exit function
 */
static void __exit l2cpu_exit(void)
{
	platform_driver_unregister(&l2cpu_noc_driver);
	platform_device_unregister(l2cpu_noc_pdev);
}

module_init(l2cpu_init);
module_exit(l2cpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tenstorrent Inc.");
MODULE_DESCRIPTION("L2CPU NOC Window Management Driver");
