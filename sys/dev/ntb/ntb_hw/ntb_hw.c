/*-
 * Copyright (C) 2013 Intel Corporation
 * Copyright (C) 2015 EMC Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <machine/bus.h>
#include <machine/pmap.h>
#include <machine/resource.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include "ntb_regs.h"
#include "ntb_hw.h"

/*
 * The Non-Transparent Bridge (NTB) is a device on some Intel processors that
 * allows you to connect two systems using a PCI-e link.
 *
 * This module contains the hardware abstraction layer for the NTB. It allows
 * you to send and recieve interrupts, map the memory windows and send and
 * receive messages in the scratch-pad registers.
 *
 * NOTE: Much of the code in this module is shared with Linux. Any patches may
 * be picked up and redistributed in Linux with a dual GPL/BSD license.
 */

#define MAX_MSIX_INTERRUPTS MAX(XEON_DB_COUNT, SOC_DB_COUNT)

#define NTB_HB_TIMEOUT		1 /* second */
#define SOC_LINK_RECOVERY_TIME	500 /* ms */

#define DEVICE2SOFTC(dev) ((struct ntb_softc *) device_get_softc(dev))

enum ntb_device_type {
	NTB_XEON,
	NTB_SOC
};

enum ntb_bar {
	NTB_CONFIG_BAR = 0,
	NTB_B2B_BAR_1,
	NTB_B2B_BAR_2,
	NTB_B2B_BAR_3,
	NTB_MAX_BARS
};

/* Device features and workarounds */
#define HAS_FEATURE(feature)	\
	((ntb->features & (feature)) != 0)

struct ntb_hw_info {
	uint32_t		device_id;
	const char		*desc;
	enum ntb_device_type	type;
	uint32_t		features;
};

struct ntb_pci_bar_info {
	bus_space_tag_t		pci_bus_tag;
	bus_space_handle_t	pci_bus_handle;
	int			pci_resource_id;
	struct resource		*pci_resource;
	vm_paddr_t		pbase;
	void			*vbase;
	u_long			size;

	/* Configuration register offsets */
	uint32_t		psz_off;
	uint32_t		ssz_off;
	uint32_t		pbarxlat_off;
};

struct ntb_int_info {
	struct resource	*res;
	int		rid;
	void		*tag;
};

struct ntb_vec {
	struct ntb_softc	*ntb;
	uint32_t		num;
};

struct ntb_reg {
	uint32_t	ntb_ctl;
	uint32_t	lnk_sta;
	uint8_t		db_size;
	unsigned	mw_bar[NTB_MAX_BARS];
};

struct ntb_alt_reg {
	uint32_t	db_bell;
	uint32_t	db_mask;
	uint32_t	spad;
};

struct ntb_xlat_reg {
	uint32_t	bar0_base;
	uint32_t	bar2_base;
	uint32_t	bar4_base;
	uint32_t	bar5_base;

	uint32_t	bar2_xlat;
	uint32_t	bar4_xlat;
	uint32_t	bar5_xlat;

	uint32_t	bar2_limit;
	uint32_t	bar4_limit;
	uint32_t	bar5_limit;
};

struct ntb_b2b_addr {
	uint64_t	bar0_addr;
	uint64_t	bar2_addr64;
	uint64_t	bar4_addr64;
	uint64_t	bar4_addr32;
	uint64_t	bar5_addr32;
};

struct ntb_softc {
	device_t		device;
	enum ntb_device_type	type;
	uint64_t		features;

	struct ntb_pci_bar_info	bar_info[NTB_MAX_BARS];
	struct ntb_int_info	int_info[MAX_MSIX_INTERRUPTS];
	uint32_t		allocated_interrupts;

	struct callout		heartbeat_timer;
	struct callout		lr_timer;

	void			*ntb_ctx;
	const struct ntb_ctx_ops *ctx_ops;
	struct ntb_vec		*msix_vec;
#define CTX_LOCK(sc)		mtx_lock_spin(&(sc)->ctx_lock)
#define CTX_UNLOCK(sc)		mtx_unlock_spin(&(sc)->ctx_lock)
#define CTX_ASSERT(sc,f)	mtx_assert(&(sc)->ctx_lock, (f))
	struct mtx		ctx_lock;

	struct {
		uint32_t ldb;
		uint32_t ldb_mask;
		uint32_t bar4_xlat;
		uint32_t bar5_xlat;
		uint32_t spad_local;
		uint32_t spci_cmd;
	} reg_ofs;
	uint32_t ppd;
	uint8_t conn_type;
	uint8_t dev_type;
	uint8_t link_width;
	uint8_t link_speed;

	/* Offset of peer bar0 in B2B BAR */
	uint64_t			b2b_off;
	/* Memory window used to access peer bar0 */
#define B2B_MW_DISABLED			UINT8_MAX
	uint8_t				b2b_mw_idx;

	uint8_t				mw_count;
	uint8_t				spad_count;
	uint8_t				db_count;
	uint8_t				db_vec_count;
	uint8_t				db_vec_shift;

	/* Protects local db_mask. */
#define DB_MASK_LOCK(sc)	mtx_lock_spin(&(sc)->db_mask_lock)
#define DB_MASK_UNLOCK(sc)	mtx_unlock_spin(&(sc)->db_mask_lock)
#define DB_MASK_ASSERT(sc,f)	mtx_assert(&(sc)->db_mask_lock, (f))
	struct mtx			db_mask_lock;

	uint32_t			ntb_ctl;
	uint32_t			lnk_sta;

	uint64_t			db_valid_mask;
	uint64_t			db_link_mask;
	uint64_t			db_mask;

	int				last_ts;	/* ticks @ last irq */

	const struct ntb_reg		*reg;
	const struct ntb_alt_reg	*self_reg;
	const struct ntb_alt_reg	*peer_reg;
	const struct ntb_xlat_reg	*xlat_reg;
};

#ifdef __i386__
static __inline uint64_t
bus_space_read_8(bus_space_tag_t tag, bus_space_handle_t handle,
    bus_size_t offset)
{

	return (bus_space_read_4(tag, handle, offset) |
	    ((uint64_t)bus_space_read_4(tag, handle, offset + 4)) << 32);
}

static __inline void
bus_space_write_8(bus_space_tag_t tag, bus_space_handle_t handle,
    bus_size_t offset, uint64_t val)
{

	bus_space_write_4(tag, handle, offset, val);
	bus_space_write_4(tag, handle, offset + 4, val >> 32);
}
#endif

#define ntb_bar_read(SIZE, bar, offset) \
	    bus_space_read_ ## SIZE (ntb->bar_info[(bar)].pci_bus_tag, \
	    ntb->bar_info[(bar)].pci_bus_handle, (offset))
#define ntb_bar_write(SIZE, bar, offset, val) \
	    bus_space_write_ ## SIZE (ntb->bar_info[(bar)].pci_bus_tag, \
	    ntb->bar_info[(bar)].pci_bus_handle, (offset), (val))
#define ntb_reg_read(SIZE, offset) ntb_bar_read(SIZE, NTB_CONFIG_BAR, offset)
#define ntb_reg_write(SIZE, offset, val) \
	    ntb_bar_write(SIZE, NTB_CONFIG_BAR, offset, val)
#define ntb_mw_read(SIZE, offset) \
	    ntb_bar_read(SIZE, ntb_mw_to_bar(ntb, ntb->b2b_mw_idx), offset)
#define ntb_mw_write(SIZE, offset, val) \
	    ntb_bar_write(SIZE, ntb_mw_to_bar(ntb, ntb->b2b_mw_idx), \
		offset, val)

static int ntb_probe(device_t device);
static int ntb_attach(device_t device);
static int ntb_detach(device_t device);
static inline enum ntb_bar ntb_mw_to_bar(struct ntb_softc *, unsigned mw);
static inline bool bar_is_64bit(struct ntb_softc *, enum ntb_bar);
static inline void bar_get_xlat_params(struct ntb_softc *, enum ntb_bar,
    uint32_t *base, uint32_t *xlat, uint32_t *lmt);
static int ntb_map_pci_bars(struct ntb_softc *ntb);
static void print_map_success(struct ntb_softc *, struct ntb_pci_bar_info *);
static int map_mmr_bar(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar);
static int map_memory_window_bar(struct ntb_softc *ntb,
    struct ntb_pci_bar_info *bar);
static void ntb_unmap_pci_bar(struct ntb_softc *ntb);
static int ntb_remap_msix(device_t, uint32_t desired, uint32_t avail);
static int ntb_init_isr(struct ntb_softc *ntb);
static int ntb_setup_legacy_interrupt(struct ntb_softc *ntb);
static int ntb_setup_msix(struct ntb_softc *ntb, uint32_t num_vectors);
static void ntb_teardown_interrupts(struct ntb_softc *ntb);
static inline uint64_t ntb_vec_mask(struct ntb_softc *, uint64_t db_vector);
static void ntb_interrupt(struct ntb_softc *, uint32_t vec);
static void ndev_vec_isr(void *arg);
static void ndev_irq_isr(void *arg);
static inline uint64_t db_ioread(struct ntb_softc *, uint64_t regoff);
static inline void db_iowrite(struct ntb_softc *, uint64_t regoff, uint64_t val);
static int ntb_create_msix_vec(struct ntb_softc *ntb, uint32_t num_vectors);
static void ntb_free_msix_vec(struct ntb_softc *ntb);
static struct ntb_hw_info *ntb_get_device_info(uint32_t device_id);
static void ntb_detect_max_mw(struct ntb_softc *ntb);
static int ntb_detect_xeon(struct ntb_softc *ntb);
static int ntb_detect_soc(struct ntb_softc *ntb);
static int ntb_xeon_init_dev(struct ntb_softc *ntb);
static int ntb_soc_init_dev(struct ntb_softc *ntb);
static void ntb_teardown_xeon(struct ntb_softc *ntb);
static void configure_soc_secondary_side_bars(struct ntb_softc *ntb);
static void xeon_reset_sbar_size(struct ntb_softc *, enum ntb_bar idx,
    enum ntb_bar regbar);
static void xeon_set_sbar_base_and_limit(struct ntb_softc *,
    uint64_t base_addr, enum ntb_bar idx, enum ntb_bar regbar);
static void xeon_set_pbar_xlat(struct ntb_softc *, uint64_t base_addr,
    enum ntb_bar idx);
static int xeon_setup_b2b_mw(struct ntb_softc *,
    const struct ntb_b2b_addr *addr, const struct ntb_b2b_addr *peer_addr);
static inline bool link_is_up(struct ntb_softc *ntb);
static inline bool soc_link_is_err(struct ntb_softc *ntb);
static inline enum ntb_speed ntb_link_sta_speed(struct ntb_softc *);
static inline enum ntb_width ntb_link_sta_width(struct ntb_softc *);
static void soc_link_hb(void *arg);
static void ntb_db_event(struct ntb_softc *ntb, uint32_t vec);
static void recover_soc_link(void *arg);
static bool ntb_poll_link(struct ntb_softc *ntb);
static void save_bar_parameters(struct ntb_pci_bar_info *bar);

static struct ntb_hw_info pci_ids[] = {
	{ 0x0C4E8086, "Atom Processor S1200 NTB Primary B2B", NTB_SOC, 0 },

	/* XXX: PS/SS IDs left out until they are supported. */
	{ 0x37258086, "JSF Xeon C35xx/C55xx Non-Transparent Bridge B2B",
		NTB_XEON, NTB_SDOORBELL_LOCKUP | NTB_B2BDOORBELL_BIT14 },
	{ 0x3C0D8086, "SNB Xeon E5/Core i7 Non-Transparent Bridge B2B",
		NTB_XEON, NTB_SDOORBELL_LOCKUP | NTB_B2BDOORBELL_BIT14 },
	{ 0x0E0D8086, "IVT Xeon E5 V2 Non-Transparent Bridge B2B", NTB_XEON,
		NTB_SDOORBELL_LOCKUP | NTB_B2BDOORBELL_BIT14 |
		    NTB_SB01BASE_LOCKUP | NTB_BAR_SIZE_4K },
	{ 0x2F0D8086, "HSX Xeon E5 V3 Non-Transparent Bridge B2B", NTB_XEON,
		NTB_SDOORBELL_LOCKUP | NTB_B2BDOORBELL_BIT14 |
		    NTB_SB01BASE_LOCKUP },
	{ 0x6F0D8086, "BDX Xeon E5 V4 Non-Transparent Bridge B2B", NTB_XEON,
		NTB_SDOORBELL_LOCKUP | NTB_B2BDOORBELL_BIT14 |
		    NTB_SB01BASE_LOCKUP },

	{ 0x00000000, NULL, NTB_SOC, 0 }
};

static const struct ntb_reg soc_reg = {
	.ntb_ctl = SOC_NTBCNTL_OFFSET,
	.lnk_sta = SOC_LINK_STATUS_OFFSET,
	.db_size = sizeof(uint64_t),
	.mw_bar = { NTB_B2B_BAR_1, NTB_B2B_BAR_2 },
};

static const struct ntb_alt_reg soc_b2b_reg = {
	.db_bell = SOC_B2B_DOORBELL_OFFSET,
	.spad = SOC_B2B_SPAD_OFFSET,
};

static const struct ntb_xlat_reg soc_sec_xlat = {
#if 0
	/* "FIXME" says the Linux driver. */
	.bar0_base = SOC_SBAR0BASE_OFFSET,
	.bar2_base = SOC_SBAR2BASE_OFFSET,
	.bar4_base = SOC_SBAR4BASE_OFFSET,

	.bar2_limit = SOC_SBAR2LMT_OFFSET,
	.bar4_limit = SOC_SBAR4LMT_OFFSET,
#endif

	.bar2_xlat = SOC_SBAR2XLAT_OFFSET,
	.bar4_xlat = SOC_SBAR4XLAT_OFFSET,
};

static const struct ntb_reg xeon_reg = {
	.ntb_ctl = XEON_NTBCNTL_OFFSET,
	.lnk_sta = XEON_LINK_STATUS_OFFSET,
	.db_size = sizeof(uint16_t),
	.mw_bar = { NTB_B2B_BAR_1, NTB_B2B_BAR_2, NTB_B2B_BAR_3 },
};

static const struct ntb_alt_reg xeon_b2b_reg = {
	.db_bell = XEON_B2B_DOORBELL_OFFSET,
	.spad = XEON_B2B_SPAD_OFFSET,
};

static const struct ntb_xlat_reg xeon_sec_xlat = {
	.bar0_base = XEON_SBAR0BASE_OFFSET,
	.bar2_base = XEON_SBAR2BASE_OFFSET,
	.bar4_base = XEON_SBAR4BASE_OFFSET,
	.bar5_base = XEON_SBAR5BASE_OFFSET,

	.bar2_limit = XEON_SBAR2LMT_OFFSET,
	.bar4_limit = XEON_SBAR4LMT_OFFSET,
	.bar5_limit = XEON_SBAR5LMT_OFFSET,

	.bar2_xlat = XEON_SBAR2XLAT_OFFSET,
	.bar4_xlat = XEON_SBAR4XLAT_OFFSET,
	.bar5_xlat = XEON_SBAR5XLAT_OFFSET,
};

static const struct ntb_b2b_addr xeon_b2b_usd_addr = {
	.bar0_addr = XEON_B2B_BAR0_USD_ADDR,
	.bar2_addr64 = XEON_B2B_BAR2_USD_ADDR64,
	.bar4_addr64 = XEON_B2B_BAR4_USD_ADDR64,
	.bar4_addr32 = XEON_B2B_BAR4_USD_ADDR32,
	.bar5_addr32 = XEON_B2B_BAR5_USD_ADDR32,
};

static const struct ntb_b2b_addr xeon_b2b_dsd_addr = {
	.bar0_addr = XEON_B2B_BAR0_DSD_ADDR,
	.bar2_addr64 = XEON_B2B_BAR2_DSD_ADDR64,
	.bar4_addr64 = XEON_B2B_BAR4_DSD_ADDR64,
	.bar4_addr32 = XEON_B2B_BAR4_DSD_ADDR32,
	.bar5_addr32 = XEON_B2B_BAR5_DSD_ADDR32,
};

/*
 * OS <-> Driver interface structures
 */
MALLOC_DEFINE(M_NTB, "ntb_hw", "ntb_hw driver memory allocations");

static device_method_t ntb_pci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,     ntb_probe),
	DEVMETHOD(device_attach,    ntb_attach),
	DEVMETHOD(device_detach,    ntb_detach),
	DEVMETHOD_END
};

static driver_t ntb_pci_driver = {
	"ntb_hw",
	ntb_pci_methods,
	sizeof(struct ntb_softc),
};

static devclass_t ntb_devclass;
DRIVER_MODULE(ntb_hw, pci, ntb_pci_driver, ntb_devclass, NULL, NULL);
MODULE_VERSION(ntb_hw, 1);

SYSCTL_NODE(_hw, OID_AUTO, ntb, CTLFLAG_RW, 0, "NTB sysctls");

/*
 * OS <-> Driver linkage functions
 */
static int
ntb_probe(device_t device)
{
	struct ntb_hw_info *p;

	p = ntb_get_device_info(pci_get_devid(device));
	if (p == NULL)
		return (ENXIO);

	device_set_desc(device, p->desc);
	return (0);
}

static int
ntb_attach(device_t device)
{
	struct ntb_softc *ntb;
	struct ntb_hw_info *p;
	int error;

	ntb = DEVICE2SOFTC(device);
	p = ntb_get_device_info(pci_get_devid(device));

	ntb->device = device;
	ntb->type = p->type;
	ntb->features = p->features;
	ntb->b2b_mw_idx = B2B_MW_DISABLED;

	/* Heartbeat timer for NTB_SOC since there is no link interrupt */
	callout_init(&ntb->heartbeat_timer, 1);
	callout_init(&ntb->lr_timer, 1);
	mtx_init(&ntb->db_mask_lock, "ntb hw bits", NULL, MTX_SPIN);
	mtx_init(&ntb->ctx_lock, "ntb ctx", NULL, MTX_SPIN);

	if (ntb->type == NTB_SOC)
		error = ntb_detect_soc(ntb);
	else
		error = ntb_detect_xeon(ntb);
	if (error)
		goto out;

	ntb_detect_max_mw(ntb);

	error = ntb_map_pci_bars(ntb);
	if (error)
		goto out;
	if (ntb->type == NTB_SOC)
		error = ntb_soc_init_dev(ntb);
	else
		error = ntb_xeon_init_dev(ntb);
	if (error)
		goto out;
	error = ntb_init_isr(ntb);
	if (error)
		goto out;

	pci_enable_busmaster(ntb->device);

out:
	if (error != 0)
		ntb_detach(device);
	return (error);
}

static int
ntb_detach(device_t device)
{
	struct ntb_softc *ntb;

	ntb = DEVICE2SOFTC(device);

	ntb_db_set_mask(ntb, ntb->db_valid_mask);
	callout_drain(&ntb->heartbeat_timer);
	callout_drain(&ntb->lr_timer);
	if (ntb->type == NTB_XEON)
		ntb_teardown_xeon(ntb);
	ntb_teardown_interrupts(ntb);

	mtx_destroy(&ntb->db_mask_lock);
	mtx_destroy(&ntb->ctx_lock);

	/*
	 * Redetect total MWs so we unmap properly -- in case we lowered the
	 * maximum to work around Xeon errata.
	 */
	ntb_detect_max_mw(ntb);
	ntb_unmap_pci_bar(ntb);

	return (0);
}

/*
 * Driver internal routines
 */
static inline enum ntb_bar
ntb_mw_to_bar(struct ntb_softc *ntb, unsigned mw)
{

	KASSERT(mw < ntb->mw_count ||
	    (mw != B2B_MW_DISABLED && mw == ntb->b2b_mw_idx),
	    ("%s: mw:%u > count:%u", __func__, mw, (unsigned)ntb->mw_count));
	KASSERT(ntb->reg->mw_bar[mw] != 0, ("invalid mw"));

	return (ntb->reg->mw_bar[mw]);
}

static inline bool
bar_is_64bit(struct ntb_softc *ntb, enum ntb_bar bar)
{
	/* XXX This assertion could be stronger. */
	KASSERT(bar < NTB_MAX_BARS, ("bogus bar"));
	return (bar < NTB_B2B_BAR_2 || !HAS_FEATURE(NTB_SPLIT_BAR));
}

static inline void
bar_get_xlat_params(struct ntb_softc *ntb, enum ntb_bar bar, uint32_t *base,
    uint32_t *xlat, uint32_t *lmt)
{
	uint32_t basev, lmtv, xlatv;

	switch (bar) {
	case NTB_B2B_BAR_1:
		basev = ntb->xlat_reg->bar2_base;
		lmtv = ntb->xlat_reg->bar2_limit;
		xlatv = ntb->xlat_reg->bar2_xlat;
		break;
	case NTB_B2B_BAR_2:
		basev = ntb->xlat_reg->bar4_base;
		lmtv = ntb->xlat_reg->bar4_limit;
		xlatv = ntb->xlat_reg->bar4_xlat;
		break;
	case NTB_B2B_BAR_3:
		basev = ntb->xlat_reg->bar5_base;
		lmtv = ntb->xlat_reg->bar5_limit;
		xlatv = ntb->xlat_reg->bar5_xlat;
		break;
	default:
		KASSERT(bar >= NTB_B2B_BAR_1 && bar < NTB_MAX_BARS,
		    ("bad bar"));
		basev = lmtv = xlatv = 0;
		break;
	}

	if (base != NULL)
		*base = basev;
	if (xlat != NULL)
		*xlat = xlatv;
	if (lmt != NULL)
		*lmt = lmtv;
}

static int
ntb_map_pci_bars(struct ntb_softc *ntb)
{
	int rc;

	ntb->bar_info[NTB_CONFIG_BAR].pci_resource_id = PCIR_BAR(0);
	rc = map_mmr_bar(ntb, &ntb->bar_info[NTB_CONFIG_BAR]);
	if (rc != 0)
		goto out;

	ntb->bar_info[NTB_B2B_BAR_1].pci_resource_id = PCIR_BAR(2);
	rc = map_memory_window_bar(ntb, &ntb->bar_info[NTB_B2B_BAR_1]);
	if (rc != 0)
		goto out;
	ntb->bar_info[NTB_B2B_BAR_1].psz_off = XEON_PBAR23SZ_OFFSET;
	ntb->bar_info[NTB_B2B_BAR_1].ssz_off = XEON_SBAR23SZ_OFFSET;
	ntb->bar_info[NTB_B2B_BAR_1].pbarxlat_off = XEON_PBAR2XLAT_OFFSET;

	ntb->bar_info[NTB_B2B_BAR_2].pci_resource_id = PCIR_BAR(4);
	/* XXX Are shared MW B2Bs write-combining? */
	if (HAS_FEATURE(NTB_SDOORBELL_LOCKUP) && !HAS_FEATURE(NTB_SPLIT_BAR))
		rc = map_mmr_bar(ntb, &ntb->bar_info[NTB_B2B_BAR_2]);
	else
		rc = map_memory_window_bar(ntb, &ntb->bar_info[NTB_B2B_BAR_2]);
	ntb->bar_info[NTB_B2B_BAR_2].psz_off = XEON_PBAR4SZ_OFFSET;
	ntb->bar_info[NTB_B2B_BAR_2].ssz_off = XEON_SBAR4SZ_OFFSET;
	ntb->bar_info[NTB_B2B_BAR_2].pbarxlat_off = XEON_PBAR4XLAT_OFFSET;

	if (!HAS_FEATURE(NTB_SPLIT_BAR))
		goto out;

	ntb->bar_info[NTB_B2B_BAR_3].pci_resource_id = PCIR_BAR(5);
	if (HAS_FEATURE(NTB_SDOORBELL_LOCKUP))
		rc = map_mmr_bar(ntb, &ntb->bar_info[NTB_B2B_BAR_3]);
	else
		rc = map_memory_window_bar(ntb, &ntb->bar_info[NTB_B2B_BAR_3]);
	ntb->bar_info[NTB_B2B_BAR_3].psz_off = XEON_PBAR5SZ_OFFSET;
	ntb->bar_info[NTB_B2B_BAR_3].ssz_off = XEON_SBAR5SZ_OFFSET;
	ntb->bar_info[NTB_B2B_BAR_3].pbarxlat_off = XEON_PBAR5XLAT_OFFSET;

out:
	if (rc != 0)
		device_printf(ntb->device,
		    "unable to allocate pci resource\n");
	return (rc);
}

static void
print_map_success(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar)
{

	device_printf(ntb->device, "Bar size = %lx, v %p, p %p\n",
	    bar->size, bar->vbase, (void *)(bar->pbase));
}

static int
map_mmr_bar(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar)
{

	bar->pci_resource = bus_alloc_resource_any(ntb->device, SYS_RES_MEMORY,
	    &bar->pci_resource_id, RF_ACTIVE);
	if (bar->pci_resource == NULL)
		return (ENXIO);

	save_bar_parameters(bar);
	print_map_success(ntb, bar);
	return (0);
}

static int
map_memory_window_bar(struct ntb_softc *ntb, struct ntb_pci_bar_info *bar)
{
	int rc;
	uint8_t bar_size_bits = 0;

	bar->pci_resource = bus_alloc_resource_any(ntb->device, SYS_RES_MEMORY,
	    &bar->pci_resource_id, RF_ACTIVE);

	if (bar->pci_resource == NULL)
		return (ENXIO);

	save_bar_parameters(bar);
	/*
	 * Ivytown NTB BAR sizes are misreported by the hardware due to a
	 * hardware issue. To work around this, query the size it should be
	 * configured to by the device and modify the resource to correspond to
	 * this new size. The BIOS on systems with this problem is required to
	 * provide enough address space to allow the driver to make this change
	 * safely.
	 *
	 * Ideally I could have just specified the size when I allocated the
	 * resource like:
	 *  bus_alloc_resource(ntb->device,
	 *	SYS_RES_MEMORY, &bar->pci_resource_id, 0ul, ~0ul,
	 *	1ul << bar_size_bits, RF_ACTIVE);
	 * but the PCI driver does not honor the size in this call, so we have
	 * to modify it after the fact.
	 */
	if (HAS_FEATURE(NTB_BAR_SIZE_4K)) {
		if (bar->pci_resource_id == PCIR_BAR(2))
			bar_size_bits = pci_read_config(ntb->device,
			    XEON_PBAR23SZ_OFFSET, 1);
		else
			bar_size_bits = pci_read_config(ntb->device,
			    XEON_PBAR45SZ_OFFSET, 1);

		rc = bus_adjust_resource(ntb->device, SYS_RES_MEMORY,
		    bar->pci_resource, bar->pbase,
		    bar->pbase + (1ul << bar_size_bits) - 1);
		if (rc != 0) {
			device_printf(ntb->device,
			    "unable to resize bar\n");
			return (rc);
		}

		save_bar_parameters(bar);
	}

	/* Mark bar region as write combining to improve performance. */
	rc = pmap_change_attr((vm_offset_t)bar->vbase, bar->size,
	    VM_MEMATTR_WRITE_COMBINING);
	if (rc != 0) {
		device_printf(ntb->device,
		    "unable to mark bar as WRITE_COMBINING\n");
		return (rc);
	}
	print_map_success(ntb, bar);
	return (0);
}

static void
ntb_unmap_pci_bar(struct ntb_softc *ntb)
{
	struct ntb_pci_bar_info *current_bar;
	int i;

	for (i = 0; i < NTB_MAX_BARS; i++) {
		current_bar = &ntb->bar_info[i];
		if (current_bar->pci_resource != NULL)
			bus_release_resource(ntb->device, SYS_RES_MEMORY,
			    current_bar->pci_resource_id,
			    current_bar->pci_resource);
	}
}

static int
ntb_setup_msix(struct ntb_softc *ntb, uint32_t num_vectors)
{
	uint32_t i;
	int rc;

	for (i = 0; i < num_vectors; i++) {
		ntb->int_info[i].rid = i + 1;
		ntb->int_info[i].res = bus_alloc_resource_any(ntb->device,
		    SYS_RES_IRQ, &ntb->int_info[i].rid, RF_ACTIVE);
		if (ntb->int_info[i].res == NULL) {
			device_printf(ntb->device,
			    "bus_alloc_resource failed\n");
			return (ENOMEM);
		}
		ntb->int_info[i].tag = NULL;
		ntb->allocated_interrupts++;
		rc = bus_setup_intr(ntb->device, ntb->int_info[i].res,
		    INTR_MPSAFE | INTR_TYPE_MISC, NULL, ndev_vec_isr,
		    &ntb->msix_vec[i], &ntb->int_info[i].tag);
		if (rc != 0) {
			device_printf(ntb->device, "bus_setup_intr failed\n");
			return (ENXIO);
		}
	}
	return (0);
}

/*
 * The Linux NTB driver drops from MSI-X to legacy INTx if a unique vector
 * cannot be allocated for each MSI-X message.  JHB seems to think remapping
 * should be okay.  This tunable should enable us to test that hypothesis
 * when someone gets their hands on some Xeon hardware.
 */
static int ntb_force_remap_mode;
SYSCTL_INT(_hw_ntb, OID_AUTO, force_remap_mode, CTLFLAG_RDTUN,
    &ntb_force_remap_mode, 0, "If enabled, force MSI-X messages to be remapped"
    " to a smaller number of ithreads, even if the desired number are "
    "available");

/*
 * In case it is NOT ok, give consumers an abort button.
 */
static int ntb_prefer_intx;
SYSCTL_INT(_hw_ntb, OID_AUTO, prefer_intx_to_remap, CTLFLAG_RDTUN,
    &ntb_prefer_intx, 0, "If enabled, prefer to use legacy INTx mode rather "
    "than remapping MSI-X messages over available slots (match Linux driver "
    "behavior)");

/*
 * Remap the desired number of MSI-X messages to available ithreads in a simple
 * round-robin fashion.
 */
static int
ntb_remap_msix(device_t dev, uint32_t desired, uint32_t avail)
{
	u_int *vectors;
	uint32_t i;
	int rc;

	if (ntb_prefer_intx != 0)
		return (ENXIO);

	vectors = malloc(desired * sizeof(*vectors), M_NTB, M_ZERO | M_WAITOK);

	for (i = 0; i < desired; i++)
		vectors[i] = (i % avail) + 1;

	rc = pci_remap_msix(dev, desired, vectors);
	free(vectors, M_NTB);
	return (rc);
}

static int
ntb_init_isr(struct ntb_softc *ntb)
{
	uint32_t desired_vectors, num_vectors;
	int rc;

	ntb->allocated_interrupts = 0;
	ntb->last_ts = ticks;

	/*
	 * Mask all doorbell interrupts.
	 */
	ntb_db_set_mask(ntb, ntb->db_valid_mask);

	num_vectors = desired_vectors = MIN(pci_msix_count(ntb->device),
	    ntb->db_count);
	if (desired_vectors >= 1) {
		rc = pci_alloc_msix(ntb->device, &num_vectors);

		if (ntb_force_remap_mode != 0 && rc == 0 &&
		    num_vectors == desired_vectors)
			num_vectors--;

		if (rc == 0 && num_vectors < desired_vectors) {
			rc = ntb_remap_msix(ntb->device, desired_vectors,
			    num_vectors);
			if (rc == 0)
				num_vectors = desired_vectors;
			else
				pci_release_msi(ntb->device);
		}
		if (rc != 0)
			num_vectors = 1;
	} else
		num_vectors = 1;

	if (ntb->type == NTB_XEON && num_vectors < ntb->db_vec_count) {
		ntb->db_vec_count = 1;
		ntb->db_vec_shift = ntb->db_count;
		rc = ntb_setup_legacy_interrupt(ntb);
	} else {
		ntb_create_msix_vec(ntb, num_vectors);
		rc = ntb_setup_msix(ntb, num_vectors);
	}
	if (rc != 0) {
		device_printf(ntb->device,
		    "Error allocating interrupts: %d\n", rc);
		ntb_free_msix_vec(ntb);
	}

	return (rc);
}

static int
ntb_setup_legacy_interrupt(struct ntb_softc *ntb)
{
	int rc;

	ntb->int_info[0].rid = 0;
	ntb->int_info[0].res = bus_alloc_resource_any(ntb->device, SYS_RES_IRQ,
	    &ntb->int_info[0].rid, RF_SHAREABLE|RF_ACTIVE);
	if (ntb->int_info[0].res == NULL) {
		device_printf(ntb->device, "bus_alloc_resource failed\n");
		return (ENOMEM);
	}

	ntb->int_info[0].tag = NULL;
	ntb->allocated_interrupts = 1;

	rc = bus_setup_intr(ntb->device, ntb->int_info[0].res,
	    INTR_MPSAFE | INTR_TYPE_MISC, NULL, ndev_irq_isr,
	    ntb, &ntb->int_info[0].tag);
	if (rc != 0) {
		device_printf(ntb->device, "bus_setup_intr failed\n");
		return (ENXIO);
	}

	return (0);
}

static void
ntb_teardown_interrupts(struct ntb_softc *ntb)
{
	struct ntb_int_info *current_int;
	int i;

	for (i = 0; i < ntb->allocated_interrupts; i++) {
		current_int = &ntb->int_info[i];
		if (current_int->tag != NULL)
			bus_teardown_intr(ntb->device, current_int->res,
			    current_int->tag);

		if (current_int->res != NULL)
			bus_release_resource(ntb->device, SYS_RES_IRQ,
			    rman_get_rid(current_int->res), current_int->res);
	}

	ntb_free_msix_vec(ntb);
	pci_release_msi(ntb->device);
}

/*
 * Doorbell register and mask are 64-bit on SoC, 16-bit on Xeon.  Abstract it
 * out to make code clearer.
 */
static inline uint64_t
db_ioread(struct ntb_softc *ntb, uint64_t regoff)
{

	if (ntb->type == NTB_SOC)
		return (ntb_reg_read(8, regoff));

	KASSERT(ntb->type == NTB_XEON, ("bad ntb type"));

	return (ntb_reg_read(2, regoff));
}

static inline void
db_iowrite(struct ntb_softc *ntb, uint64_t regoff, uint64_t val)
{

	KASSERT((val & ~ntb->db_valid_mask) == 0,
	    ("%s: Invalid bits 0x%jx (valid: 0x%jx)", __func__,
	     (uintmax_t)(val & ~ntb->db_valid_mask),
	     (uintmax_t)ntb->db_valid_mask));

	if (regoff == ntb->reg_ofs.ldb_mask)
		DB_MASK_ASSERT(ntb, MA_OWNED);

	if (ntb->type == NTB_SOC) {
		ntb_reg_write(8, regoff, val);
		return;
	}

	KASSERT(ntb->type == NTB_XEON, ("bad ntb type"));
	ntb_reg_write(2, regoff, (uint16_t)val);
}

void
ntb_db_set_mask(struct ntb_softc *ntb, uint64_t bits)
{

	DB_MASK_LOCK(ntb);
	ntb->db_mask |= bits;
	db_iowrite(ntb, ntb->reg_ofs.ldb_mask, ntb->db_mask);
	DB_MASK_UNLOCK(ntb);
}

void
ntb_db_clear_mask(struct ntb_softc *ntb, uint64_t bits)
{

	KASSERT((bits & ~ntb->db_valid_mask) == 0,
	    ("%s: Invalid bits 0x%jx (valid: 0x%jx)", __func__,
	     (uintmax_t)(bits & ~ntb->db_valid_mask),
	     (uintmax_t)ntb->db_valid_mask));

	DB_MASK_LOCK(ntb);
	ntb->db_mask &= ~bits;
	db_iowrite(ntb, ntb->reg_ofs.ldb_mask, ntb->db_mask);
	DB_MASK_UNLOCK(ntb);
}

uint64_t
ntb_db_read(struct ntb_softc *ntb)
{

	return (db_ioread(ntb, ntb->reg_ofs.ldb));
}

void
ntb_db_clear(struct ntb_softc *ntb, uint64_t bits)
{

	KASSERT((bits & ~ntb->db_valid_mask) == 0,
	    ("%s: Invalid bits 0x%jx (valid: 0x%jx)", __func__,
	     (uintmax_t)(bits & ~ntb->db_valid_mask),
	     (uintmax_t)ntb->db_valid_mask));

	db_iowrite(ntb, ntb->reg_ofs.ldb, bits);
}

static inline uint64_t
ntb_vec_mask(struct ntb_softc *ntb, uint64_t db_vector)
{
	uint64_t shift, mask;

	shift = ntb->db_vec_shift;
	mask = (1ull << shift) - 1;
	return (mask << (shift * db_vector));
}

static void
ntb_interrupt(struct ntb_softc *ntb, uint32_t vec)
{
	uint64_t vec_mask;

	ntb->last_ts = ticks;
	vec_mask = ntb_vec_mask(ntb, vec);

	if ((vec_mask & ntb->db_link_mask) != 0) {
		if (ntb_poll_link(ntb))
			ntb_link_event(ntb);
	}

	if ((vec_mask & ntb->db_valid_mask) != 0)
		ntb_db_event(ntb, vec);
}

static void
ndev_vec_isr(void *arg)
{
	struct ntb_vec *nvec = arg;

	ntb_interrupt(nvec->ntb, nvec->num);
}

static void
ndev_irq_isr(void *arg)
{
	/* If we couldn't set up MSI-X, we only have the one vector. */
	ntb_interrupt(arg, 0);
}

static int
ntb_create_msix_vec(struct ntb_softc *ntb, uint32_t num_vectors)
{
	uint32_t i;

	ntb->msix_vec = malloc(num_vectors * sizeof(*ntb->msix_vec), M_NTB,
	    M_ZERO | M_WAITOK);
	for (i = 0; i < num_vectors; i++) {
		ntb->msix_vec[i].num = i;
		ntb->msix_vec[i].ntb = ntb;
	}

	return (0);
}

static void
ntb_free_msix_vec(struct ntb_softc *ntb)
{

	if (ntb->msix_vec == NULL)
		return;

	free(ntb->msix_vec, M_NTB);
	ntb->msix_vec = NULL;
}

static struct ntb_hw_info *
ntb_get_device_info(uint32_t device_id)
{
	struct ntb_hw_info *ep = pci_ids;

	while (ep->device_id) {
		if (ep->device_id == device_id)
			return (ep);
		++ep;
	}
	return (NULL);
}

static void
ntb_teardown_xeon(struct ntb_softc *ntb)
{

	ntb_link_disable(ntb);
}

static void
ntb_detect_max_mw(struct ntb_softc *ntb)
{

	if (ntb->type == NTB_SOC) {
		ntb->mw_count = SOC_MW_COUNT;
		return;
	}

	if (HAS_FEATURE(NTB_SPLIT_BAR))
		ntb->mw_count = XEON_HSX_SPLIT_MW_COUNT;
	else
		ntb->mw_count = XEON_SNB_MW_COUNT;
}

static int
ntb_detect_xeon(struct ntb_softc *ntb)
{
	uint8_t ppd, conn_type;

	ppd = pci_read_config(ntb->device, NTB_PPD_OFFSET, 1);
	ntb->ppd = ppd;

	if ((ppd & XEON_PPD_DEV_TYPE) != 0)
		ntb->dev_type = NTB_DEV_USD;
	else
		ntb->dev_type = NTB_DEV_DSD;

	if ((ppd & XEON_PPD_SPLIT_BAR) != 0)
		ntb->features |= NTB_SPLIT_BAR;

	/* SB01BASE_LOCKUP errata is a superset of SDOORBELL errata */
	if (HAS_FEATURE(NTB_SB01BASE_LOCKUP))
		ntb->features |= NTB_SDOORBELL_LOCKUP;

	conn_type = ppd & XEON_PPD_CONN_TYPE;
	switch (conn_type) {
	case NTB_CONN_B2B:
		ntb->conn_type = conn_type;
		break;
	case NTB_CONN_RP:
	case NTB_CONN_TRANSPARENT:
	default:
		device_printf(ntb->device, "Unsupported connection type: %u\n",
		    (unsigned)conn_type);
		return (ENXIO);
	}
	return (0);
}

static int
ntb_detect_soc(struct ntb_softc *ntb)
{
	uint32_t ppd, conn_type;

	ppd = pci_read_config(ntb->device, NTB_PPD_OFFSET, 4);
	ntb->ppd = ppd;

	if ((ppd & SOC_PPD_DEV_TYPE) != 0)
		ntb->dev_type = NTB_DEV_DSD;
	else
		ntb->dev_type = NTB_DEV_USD;

	conn_type = (ppd & SOC_PPD_CONN_TYPE) >> 8;
	switch (conn_type) {
	case NTB_CONN_B2B:
		ntb->conn_type = conn_type;
		break;
	default:
		device_printf(ntb->device, "Unsupported NTB configuration\n");
		return (ENXIO);
	}
	return (0);
}

static int
ntb_xeon_init_dev(struct ntb_softc *ntb)
{
	int rc;

	ntb->reg_ofs.ldb	= XEON_PDOORBELL_OFFSET;
	ntb->reg_ofs.ldb_mask	= XEON_PDBMSK_OFFSET;
	ntb->reg_ofs.spad_local	= XEON_SPAD_OFFSET;
	ntb->reg_ofs.bar4_xlat	= XEON_SBAR4XLAT_OFFSET;
	if (HAS_FEATURE(NTB_SPLIT_BAR))
		ntb->reg_ofs.bar5_xlat = XEON_SBAR5XLAT_OFFSET;
	ntb->reg_ofs.spci_cmd	= XEON_PCICMD_OFFSET;

	ntb->spad_count		= XEON_SPAD_COUNT;
	ntb->db_count		= XEON_DB_COUNT;
	ntb->db_link_mask	= XEON_DB_LINK_BIT;
	ntb->db_vec_count	= XEON_DB_MSIX_VECTOR_COUNT;
	ntb->db_vec_shift	= XEON_DB_MSIX_VECTOR_SHIFT;

	if (ntb->conn_type != NTB_CONN_B2B) {
		device_printf(ntb->device, "Connection type %d not supported\n",
		    ntb->conn_type);
		return (ENXIO);
	}

	ntb->reg = &xeon_reg;
	ntb->peer_reg = &xeon_b2b_reg;
	ntb->xlat_reg = &xeon_sec_xlat;

	/*
	 * There is a Xeon hardware errata related to writes to SDOORBELL or
	 * B2BDOORBELL in conjunction with inbound access to NTB MMIO space,
	 * which may hang the system.  To workaround this use the second memory
	 * window to access the interrupt and scratch pad registers on the
	 * remote system.
	 */
	if (HAS_FEATURE(NTB_SDOORBELL_LOCKUP))
		/* Use the last MW for mapping remote spad */
		ntb->b2b_mw_idx = ntb->mw_count - 1;
	else if (HAS_FEATURE(NTB_B2BDOORBELL_BIT14))
		/*
		 * HW Errata on bit 14 of b2bdoorbell register.  Writes will not be
		 * mirrored to the remote system.  Shrink the number of bits by one,
		 * since bit 14 is the last bit.
		 *
		 * On REGS_THRU_MW errata mode, we don't use the b2bdoorbell register
		 * anyway.  Nor for non-B2B connection types.
		 */
		ntb->db_count = XEON_DB_COUNT - 1;

	ntb->db_valid_mask = (1ull << ntb->db_count) - 1;

	if (ntb->dev_type == NTB_DEV_USD)
		rc = xeon_setup_b2b_mw(ntb, &xeon_b2b_dsd_addr,
		    &xeon_b2b_usd_addr);
	else
		rc = xeon_setup_b2b_mw(ntb, &xeon_b2b_usd_addr,
		    &xeon_b2b_dsd_addr);
	if (rc != 0)
		return (rc);

	/* Enable Bus Master and Memory Space on the secondary side */
	ntb_reg_write(2, ntb->reg_ofs.spci_cmd,
	    PCIM_CMD_MEMEN | PCIM_CMD_BUSMASTEREN);

	/* Enable link training */
	ntb_link_enable(ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);

	return (0);
}

static int
ntb_soc_init_dev(struct ntb_softc *ntb)
{

	KASSERT(ntb->conn_type == NTB_CONN_B2B,
	    ("Unsupported NTB configuration (%d)\n", ntb->conn_type));

	ntb->reg_ofs.ldb	 = SOC_PDOORBELL_OFFSET;
	ntb->reg_ofs.ldb_mask	 = SOC_PDBMSK_OFFSET;
	ntb->reg_ofs.bar4_xlat	 = SOC_SBAR4XLAT_OFFSET;
	ntb->reg_ofs.spad_local	 = SOC_SPAD_OFFSET;
	ntb->reg_ofs.spci_cmd	 = SOC_PCICMD_OFFSET;

	ntb->spad_count		 = SOC_SPAD_COUNT;
	ntb->db_count		 = SOC_DB_COUNT;
	ntb->db_vec_count	 = SOC_DB_MSIX_VECTOR_COUNT;
	ntb->db_vec_shift	 = SOC_DB_MSIX_VECTOR_SHIFT;
	ntb->db_valid_mask	 = (1ull << ntb->db_count) - 1;

	ntb->reg = &soc_reg;
	ntb->peer_reg = &soc_b2b_reg;
	ntb->xlat_reg = &soc_sec_xlat;

	/*
	 * FIXME - MSI-X bug on early SOC HW, remove once internal issue is
	 * resolved.  Mask transaction layer internal parity errors.
	 */
	pci_write_config(ntb->device, 0xFC, 0x4, 4);

	configure_soc_secondary_side_bars(ntb);

	/* Enable Bus Master and Memory Space on the secondary side */
	ntb_reg_write(2, ntb->reg_ofs.spci_cmd,
	    PCIM_CMD_MEMEN | PCIM_CMD_BUSMASTEREN);

	/* Initiate PCI-E link training */
	ntb_link_enable(ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);

	callout_reset(&ntb->heartbeat_timer, 0, soc_link_hb, ntb);

	return (0);
}

/* XXX: Linux driver doesn't seem to do any of this for SoC. */
static void
configure_soc_secondary_side_bars(struct ntb_softc *ntb)
{

	if (ntb->dev_type == NTB_DEV_USD) {
		ntb_reg_write(8, SOC_PBAR2XLAT_OFFSET,
		    XEON_B2B_BAR2_DSD_ADDR64);
		ntb_reg_write(8, SOC_PBAR4XLAT_OFFSET,
		    XEON_B2B_BAR4_DSD_ADDR64);
		ntb_reg_write(8, SOC_MBAR23_OFFSET, XEON_B2B_BAR2_USD_ADDR64);
		ntb_reg_write(8, SOC_MBAR45_OFFSET, XEON_B2B_BAR4_USD_ADDR64);
	} else {
		ntb_reg_write(8, SOC_PBAR2XLAT_OFFSET,
		    XEON_B2B_BAR2_USD_ADDR64);
		ntb_reg_write(8, SOC_PBAR4XLAT_OFFSET,
		    XEON_B2B_BAR4_USD_ADDR64);
		ntb_reg_write(8, SOC_MBAR23_OFFSET, XEON_B2B_BAR2_DSD_ADDR64);
		ntb_reg_write(8, SOC_MBAR45_OFFSET, XEON_B2B_BAR4_DSD_ADDR64);
	}
}


/*
 * When working around Xeon SDOORBELL errata by remapping remote registers in a
 * MW, limit the B2B MW to half a MW.  By sharing a MW, half the shared MW
 * remains for use by a higher layer.
 *
 * Will only be used if working around SDOORBELL errata and the BIOS-configured
 * MW size is sufficiently large.
 */
static unsigned int ntb_b2b_mw_share;
SYSCTL_UINT(_hw_ntb, OID_AUTO, b2b_mw_share, CTLFLAG_RDTUN, &ntb_b2b_mw_share,
    0, "If enabled (non-zero), prefer to share half of the B2B peer register "
    "MW with higher level consumers.  Both sides of the NTB MUST set the same "
    "value here.");

static void
xeon_reset_sbar_size(struct ntb_softc *ntb, enum ntb_bar idx,
    enum ntb_bar regbar)
{
	struct ntb_pci_bar_info *bar;
	uint8_t bar_sz;

	if (!HAS_FEATURE(NTB_SPLIT_BAR) && idx >= NTB_B2B_BAR_3)
		return;

	bar = &ntb->bar_info[idx];
	bar_sz = pci_read_config(ntb->device, bar->psz_off, 1);
	if (idx == regbar) {
		if (ntb->b2b_off != 0)
			bar_sz--;
		else
			bar_sz = 0;
	}
	pci_write_config(ntb->device, bar->ssz_off, bar_sz, 1);
	bar_sz = pci_read_config(ntb->device, bar->ssz_off, 1);
	(void)bar_sz;
}

static void
xeon_set_sbar_base_and_limit(struct ntb_softc *ntb, uint64_t bar_addr,
    enum ntb_bar idx, enum ntb_bar regbar)
{
	uint64_t reg_val;
	uint32_t base_reg, lmt_reg;

	bar_get_xlat_params(ntb, idx, &base_reg, NULL, &lmt_reg);
	if (idx == regbar)
		bar_addr += ntb->b2b_off;

	if (!bar_is_64bit(ntb, idx)) {
		ntb_reg_write(4, base_reg, bar_addr);
		reg_val = ntb_reg_read(4, base_reg);
		(void)reg_val;

		ntb_reg_write(4, lmt_reg, bar_addr);
		reg_val = ntb_reg_read(4, lmt_reg);
		(void)reg_val;
	} else {
		ntb_reg_write(8, base_reg, bar_addr);
		reg_val = ntb_reg_read(8, base_reg);
		(void)reg_val;

		ntb_reg_write(8, lmt_reg, bar_addr);
		reg_val = ntb_reg_read(8, lmt_reg);
		(void)reg_val;
	}
}

static void
xeon_set_pbar_xlat(struct ntb_softc *ntb, uint64_t base_addr, enum ntb_bar idx)
{
	struct ntb_pci_bar_info *bar;

	bar = &ntb->bar_info[idx];
	if (HAS_FEATURE(NTB_SPLIT_BAR) && idx >= NTB_B2B_BAR_2) {
		ntb_reg_write(4, bar->pbarxlat_off, base_addr);
		base_addr = ntb_reg_read(4, bar->pbarxlat_off);
	} else {
		ntb_reg_write(8, bar->pbarxlat_off, base_addr);
		base_addr = ntb_reg_read(8, bar->pbarxlat_off);
	}
	(void)base_addr;
}

static int
xeon_setup_b2b_mw(struct ntb_softc *ntb, const struct ntb_b2b_addr *addr,
    const struct ntb_b2b_addr *peer_addr)
{
	struct ntb_pci_bar_info *b2b_bar;
	vm_size_t bar_size;
	uint64_t bar_addr;
	enum ntb_bar b2b_bar_num, i;

	if (ntb->b2b_mw_idx == B2B_MW_DISABLED) {
		b2b_bar = NULL;
		b2b_bar_num = NTB_CONFIG_BAR;
		ntb->b2b_off = 0;
	} else {
		b2b_bar_num = ntb_mw_to_bar(ntb, ntb->b2b_mw_idx);
		KASSERT(b2b_bar_num > 0 && b2b_bar_num < NTB_MAX_BARS,
		    ("invalid b2b mw bar"));

		b2b_bar = &ntb->bar_info[b2b_bar_num];
		bar_size = b2b_bar->size;

		if (ntb_b2b_mw_share != 0 &&
		    (bar_size >> 1) >= XEON_B2B_MIN_SIZE)
			ntb->b2b_off = bar_size >> 1;
		else if (bar_size >= XEON_B2B_MIN_SIZE) {
			ntb->b2b_off = 0;
			ntb->mw_count--;
		} else {
			device_printf(ntb->device,
			    "B2B bar size is too small!\n");
			return (EIO);
		}
	}

	/*
	 * Reset the secondary bar sizes to match the primary bar sizes.
	 * (Except, disable or halve the size of the B2B secondary bar.)
	 */
	for (i = NTB_B2B_BAR_1; i < NTB_MAX_BARS; i++)
		xeon_reset_sbar_size(ntb, i, b2b_bar_num);

	bar_addr = 0;
	if (b2b_bar_num == NTB_CONFIG_BAR)
		bar_addr = addr->bar0_addr;
	else if (b2b_bar_num == NTB_B2B_BAR_1)
		bar_addr = addr->bar2_addr64;
	else if (b2b_bar_num == NTB_B2B_BAR_2 && !HAS_FEATURE(NTB_SPLIT_BAR))
		bar_addr = addr->bar4_addr64;
	else if (b2b_bar_num == NTB_B2B_BAR_2)
		bar_addr = addr->bar4_addr32;
	else if (b2b_bar_num == NTB_B2B_BAR_3)
		bar_addr = addr->bar5_addr32;
	else
		KASSERT(false, ("invalid bar"));

	ntb_reg_write(8, XEON_SBAR0BASE_OFFSET, bar_addr);

	/*
	 * Other SBARs are normally hit by the PBAR xlat, except for the b2b
	 * register BAR.  The B2B BAR is either disabled above or configured
	 * half-size.  It starts at PBAR xlat + offset.
	 *
	 * Also set up incoming BAR limits == base (zero length window).
	 */
	xeon_set_sbar_base_and_limit(ntb, addr->bar2_addr64, NTB_B2B_BAR_1,
	    b2b_bar_num);
	if (HAS_FEATURE(NTB_SPLIT_BAR)) {
		xeon_set_sbar_base_and_limit(ntb, addr->bar4_addr32,
		    NTB_B2B_BAR_2, b2b_bar_num);
		xeon_set_sbar_base_and_limit(ntb, addr->bar5_addr32,
		    NTB_B2B_BAR_3, b2b_bar_num);
	} else
		xeon_set_sbar_base_and_limit(ntb, addr->bar4_addr64,
		    NTB_B2B_BAR_2, b2b_bar_num);

	/* Zero incoming translation addrs */
	ntb_reg_write(8, XEON_SBAR2XLAT_OFFSET, 0);
	ntb_reg_write(8, XEON_SBAR4XLAT_OFFSET, 0);

	/* Zero outgoing translation limits (whole bar size windows) */
	ntb_reg_write(8, XEON_PBAR2LMT_OFFSET, 0);
	ntb_reg_write(8, XEON_PBAR4LMT_OFFSET, 0);

	/* Set outgoing translation offsets */
	xeon_set_pbar_xlat(ntb, peer_addr->bar2_addr64, NTB_B2B_BAR_1);
	if (HAS_FEATURE(NTB_SPLIT_BAR)) {
		xeon_set_pbar_xlat(ntb, peer_addr->bar4_addr32, NTB_B2B_BAR_2);
		xeon_set_pbar_xlat(ntb, peer_addr->bar5_addr32, NTB_B2B_BAR_3);
	} else
		xeon_set_pbar_xlat(ntb, peer_addr->bar4_addr64, NTB_B2B_BAR_2);

	/* Set the translation offset for B2B registers */
	bar_addr = 0;
	if (b2b_bar_num == NTB_CONFIG_BAR)
		bar_addr = peer_addr->bar0_addr;
	else if (b2b_bar_num == NTB_B2B_BAR_1)
		bar_addr = peer_addr->bar2_addr64;
	else if (b2b_bar_num == NTB_B2B_BAR_2 && !HAS_FEATURE(NTB_SPLIT_BAR))
		bar_addr = peer_addr->bar4_addr64;
	else if (b2b_bar_num == NTB_B2B_BAR_2)
		bar_addr = peer_addr->bar4_addr32;
	else if (b2b_bar_num == NTB_B2B_BAR_3)
		bar_addr = peer_addr->bar5_addr32;
	else
		KASSERT(false, ("invalid bar"));

	/*
	 * B2B_XLAT_OFFSET is a 64-bit register but can only be written 32 bits
	 * at a time.
	 */
	ntb_reg_write(4, XEON_B2B_XLAT_OFFSETL, bar_addr & 0xffffffff);
	ntb_reg_write(4, XEON_B2B_XLAT_OFFSETU, bar_addr >> 32);
	return (0);
}

static inline bool
link_is_up(struct ntb_softc *ntb)
{

	if (ntb->type == NTB_XEON)
		return ((ntb->lnk_sta & NTB_LINK_STATUS_ACTIVE) != 0);

	KASSERT(ntb->type == NTB_SOC, ("ntb type"));
	return ((ntb->ntb_ctl & SOC_CNTL_LINK_DOWN) == 0);
}

static inline bool
soc_link_is_err(struct ntb_softc *ntb)
{
	uint32_t status;

	KASSERT(ntb->type == NTB_SOC, ("ntb type"));

	status = ntb_reg_read(4, SOC_LTSSMSTATEJMP_OFFSET);
	if ((status & SOC_LTSSMSTATEJMP_FORCEDETECT) != 0)
		return (true);

	status = ntb_reg_read(4, SOC_IBSTERRRCRVSTS0_OFFSET);
	return ((status & SOC_IBIST_ERR_OFLOW) != 0);
}

/* SOC does not have link status interrupt, poll on that platform */
static void
soc_link_hb(void *arg)
{
	struct ntb_softc *ntb = arg;
	sbintime_t timo, poll_ts;

	timo = NTB_HB_TIMEOUT * hz;
	poll_ts = ntb->last_ts + timo;

	/*
	 * Delay polling the link status if an interrupt was received, unless
	 * the cached link status says the link is down.
	 */
	if ((sbintime_t)ticks - poll_ts < 0 && link_is_up(ntb)) {
		timo = poll_ts - ticks;
		goto out;
	}

	if (ntb_poll_link(ntb))
		ntb_link_event(ntb);

	if (!link_is_up(ntb) && soc_link_is_err(ntb)) {
		/* Link is down with error, proceed with recovery */
		callout_reset(&ntb->lr_timer, 0, recover_soc_link, ntb);
		return;
	}

out:
	callout_reset(&ntb->heartbeat_timer, timo, soc_link_hb, ntb);
}

static void
soc_perform_link_restart(struct ntb_softc *ntb)
{
	uint32_t status;

	/* Driver resets the NTB ModPhy lanes - magic! */
	ntb_reg_write(1, SOC_MODPHY_PCSREG6, 0xe0);
	ntb_reg_write(1, SOC_MODPHY_PCSREG4, 0x40);
	ntb_reg_write(1, SOC_MODPHY_PCSREG4, 0x60);
	ntb_reg_write(1, SOC_MODPHY_PCSREG6, 0x60);

	/* Driver waits 100ms to allow the NTB ModPhy to settle */
	pause("ModPhy", hz / 10);

	/* Clear AER Errors, write to clear */
	status = ntb_reg_read(4, SOC_ERRCORSTS_OFFSET);
	status &= PCIM_AER_COR_REPLAY_ROLLOVER;
	ntb_reg_write(4, SOC_ERRCORSTS_OFFSET, status);

	/* Clear unexpected electrical idle event in LTSSM, write to clear */
	status = ntb_reg_read(4, SOC_LTSSMERRSTS0_OFFSET);
	status |= SOC_LTSSMERRSTS0_UNEXPECTEDEI;
	ntb_reg_write(4, SOC_LTSSMERRSTS0_OFFSET, status);

	/* Clear DeSkew Buffer error, write to clear */
	status = ntb_reg_read(4, SOC_DESKEWSTS_OFFSET);
	status |= SOC_DESKEWSTS_DBERR;
	ntb_reg_write(4, SOC_DESKEWSTS_OFFSET, status);

	status = ntb_reg_read(4, SOC_IBSTERRRCRVSTS0_OFFSET);
	status &= SOC_IBIST_ERR_OFLOW;
	ntb_reg_write(4, SOC_IBSTERRRCRVSTS0_OFFSET, status);

	/* Releases the NTB state machine to allow the link to retrain */
	status = ntb_reg_read(4, SOC_LTSSMSTATEJMP_OFFSET);
	status &= ~SOC_LTSSMSTATEJMP_FORCEDETECT;
	ntb_reg_write(4, SOC_LTSSMSTATEJMP_OFFSET, status);
}

/*
 * ntb_set_ctx() - associate a driver context with an ntb device
 * @ntb:        NTB device context
 * @ctx:        Driver context
 * @ctx_ops:    Driver context operations
 *
 * Associate a driver context and operations with a ntb device.  The context is
 * provided by the client driver, and the driver may associate a different
 * context with each ntb device.
 *
 * Return: Zero if the context is associated, otherwise an error number.
 */
int
ntb_set_ctx(struct ntb_softc *ntb, void *ctx, const struct ntb_ctx_ops *ops)
{

	if (ctx == NULL || ops == NULL)
		return (EINVAL);
	if (ntb->ctx_ops != NULL)
		return (EINVAL);

	CTX_LOCK(ntb);
	if (ntb->ctx_ops != NULL) {
		CTX_UNLOCK(ntb);
		return (EINVAL);
	}
	ntb->ntb_ctx = ctx;
	ntb->ctx_ops = ops;
	CTX_UNLOCK(ntb);

	return (0);
}

/*
 * It is expected that this will only be used from contexts where the ctx_lock
 * is not needed to protect ntb_ctx lifetime.
 */
void *
ntb_get_ctx(struct ntb_softc *ntb, const struct ntb_ctx_ops **ops)
{

	KASSERT(ntb->ntb_ctx != NULL && ntb->ctx_ops != NULL, ("bogus"));
	if (ops != NULL)
		*ops = ntb->ctx_ops;
	return (ntb->ntb_ctx);
}

/*
 * ntb_clear_ctx() - disassociate any driver context from an ntb device
 * @ntb:        NTB device context
 *
 * Clear any association that may exist between a driver context and the ntb
 * device.
 */
void
ntb_clear_ctx(struct ntb_softc *ntb)
{

	CTX_LOCK(ntb);
	ntb->ntb_ctx = NULL;
	ntb->ctx_ops = NULL;
	CTX_UNLOCK(ntb);
}

/*
 * ntb_link_event() - notify driver context of a change in link status
 * @ntb:        NTB device context
 *
 * Notify the driver context that the link status may have changed.  The driver
 * should call ntb_link_is_up() to get the current status.
 */
void
ntb_link_event(struct ntb_softc *ntb)
{

	CTX_LOCK(ntb);
	if (ntb->ctx_ops != NULL && ntb->ctx_ops->link_event != NULL)
		ntb->ctx_ops->link_event(ntb->ntb_ctx);
	CTX_UNLOCK(ntb);
}

/*
 * ntb_db_event() - notify driver context of a doorbell event
 * @ntb:        NTB device context
 * @vector:     Interrupt vector number
 *
 * Notify the driver context of a doorbell event.  If hardware supports
 * multiple interrupt vectors for doorbells, the vector number indicates which
 * vector received the interrupt.  The vector number is relative to the first
 * vector used for doorbells, starting at zero, and must be less than
 * ntb_db_vector_count().  The driver may call ntb_db_read() to check which
 * doorbell bits need service, and ntb_db_vector_mask() to determine which of
 * those bits are associated with the vector number.
 */
static void
ntb_db_event(struct ntb_softc *ntb, uint32_t vec)
{

	CTX_LOCK(ntb);
	if (ntb->ctx_ops != NULL && ntb->ctx_ops->db_event != NULL)
		ntb->ctx_ops->db_event(ntb->ntb_ctx, vec);
	CTX_UNLOCK(ntb);
}

/*
 * ntb_link_enable() - enable the link on the secondary side of the ntb
 * @ntb:        NTB device context
 * @max_speed:  The maximum link speed expressed as PCIe generation number[0]
 * @max_width:  The maximum link width expressed as the number of PCIe lanes[0]
 *
 * Enable the link on the secondary side of the ntb.  This can only be done
 * from the primary side of the ntb in primary or b2b topology.  The ntb device
 * should train the link to its maximum speed and width, or the requested speed
 * and width, whichever is smaller, if supported.
 *
 * Return: Zero on success, otherwise an error number.
 *
 * [0]: Only NTB_SPEED_AUTO and NTB_WIDTH_AUTO are valid inputs; other speed
 *      and width input will be ignored.
 */
int
ntb_link_enable(struct ntb_softc *ntb, enum ntb_speed s __unused,
    enum ntb_width w __unused)
{
	uint32_t cntl;

	if (ntb->type == NTB_SOC) {
		pci_write_config(ntb->device, NTB_PPD_OFFSET,
		    ntb->ppd | SOC_PPD_INIT_LINK, 4);
		return (0);
	}

	if (ntb->conn_type == NTB_CONN_TRANSPARENT) {
		ntb_link_event(ntb);
		return (0);
	}

	cntl = ntb_reg_read(4, ntb->reg->ntb_ctl);
	cntl &= ~(NTB_CNTL_LINK_DISABLE | NTB_CNTL_CFG_LOCK);
	cntl |= NTB_CNTL_P2S_BAR23_SNOOP | NTB_CNTL_S2P_BAR23_SNOOP;
	cntl |= NTB_CNTL_P2S_BAR4_SNOOP | NTB_CNTL_S2P_BAR4_SNOOP;
	if (HAS_FEATURE(NTB_SPLIT_BAR))
		cntl |= NTB_CNTL_P2S_BAR5_SNOOP | NTB_CNTL_S2P_BAR5_SNOOP;
	ntb_reg_write(4, ntb->reg->ntb_ctl, cntl);
	return (0);
}

/*
 * ntb_link_disable() - disable the link on the secondary side of the ntb
 * @ntb:        NTB device context
 *
 * Disable the link on the secondary side of the ntb.  This can only be done
 * from the primary side of the ntb in primary or b2b topology.  The ntb device
 * should disable the link.  Returning from this call must indicate that a
 * barrier has passed, though with no more writes may pass in either direction
 * across the link, except if this call returns an error number.
 *
 * Return: Zero on success, otherwise an error number.
 */
int
ntb_link_disable(struct ntb_softc *ntb)
{
	uint32_t cntl;

	if (ntb->conn_type == NTB_CONN_TRANSPARENT) {
		ntb_link_event(ntb);
		return (0);
	}

	cntl = ntb_reg_read(4, ntb->reg->ntb_ctl);
	cntl &= ~(NTB_CNTL_P2S_BAR23_SNOOP | NTB_CNTL_S2P_BAR23_SNOOP);
	cntl &= ~(NTB_CNTL_P2S_BAR4_SNOOP | NTB_CNTL_S2P_BAR4_SNOOP);
	if (HAS_FEATURE(NTB_SPLIT_BAR))
		cntl &= ~(NTB_CNTL_P2S_BAR5_SNOOP | NTB_CNTL_S2P_BAR5_SNOOP);
	cntl |= NTB_CNTL_LINK_DISABLE | NTB_CNTL_CFG_LOCK;
	ntb_reg_write(4, ntb->reg->ntb_ctl, cntl);
	return (0);
}

static void
recover_soc_link(void *arg)
{
	struct ntb_softc *ntb = arg;
	uint8_t speed, width;
	uint32_t status32;

	soc_perform_link_restart(ntb);

	/*
	 * There is a potential race between the 2 NTB devices recovering at
	 * the same time.  If the times are the same, the link will not recover
	 * and the driver will be stuck in this loop forever.  Add a random
	 * interval to the recovery time to prevent this race.
	 */
	status32 = arc4random() % SOC_LINK_RECOVERY_TIME;
	pause("Link", (SOC_LINK_RECOVERY_TIME + status32) * hz / 1000);

	status32 = ntb_reg_read(4, SOC_LTSSMSTATEJMP_OFFSET);
	if ((status32 & SOC_LTSSMSTATEJMP_FORCEDETECT) != 0)
		goto retry;

	status32 = ntb_reg_read(4, SOC_IBSTERRRCRVSTS0_OFFSET);
	if ((status32 & SOC_IBIST_ERR_OFLOW) != 0)
		goto retry;

	status32 = ntb_reg_read(4, ntb->reg->ntb_ctl);
	if ((status32 & SOC_CNTL_LINK_DOWN) != 0)
		goto out;

	status32 = ntb_reg_read(4, ntb->reg->lnk_sta);
	width = (status32 & NTB_LINK_WIDTH_MASK) >> 4;
	speed = (status32 & NTB_LINK_SPEED_MASK);
	if (ntb->link_width != width || ntb->link_speed != speed)
		goto retry;

out:
	callout_reset(&ntb->heartbeat_timer, NTB_HB_TIMEOUT * hz, soc_link_hb,
	    ntb);
	return;

retry:
	callout_reset(&ntb->lr_timer, NTB_HB_TIMEOUT * hz, recover_soc_link,
	    ntb);
}

/*
 * Polls the HW link status register(s); returns true if something has changed.
 */
static bool
ntb_poll_link(struct ntb_softc *ntb)
{
	uint32_t ntb_cntl;
	uint16_t reg_val;

	if (ntb->type == NTB_SOC) {
		ntb_cntl = ntb_reg_read(4, ntb->reg->ntb_ctl);
		if (ntb_cntl == ntb->ntb_ctl)
			return (false);

		ntb->ntb_ctl = ntb_cntl;
		ntb->lnk_sta = ntb_reg_read(4, ntb->reg->lnk_sta);
	} else {
		db_iowrite(ntb, ntb->reg_ofs.ldb, ntb->db_link_mask);

		reg_val = pci_read_config(ntb->device, ntb->reg->lnk_sta, 2);
		if (reg_val == ntb->lnk_sta)
			return (false);

		ntb->lnk_sta = reg_val;
	}
	return (true);
}

static inline enum ntb_speed
ntb_link_sta_speed(struct ntb_softc *ntb)
{

	if (!link_is_up(ntb))
		return (NTB_SPEED_NONE);
	return (ntb->lnk_sta & NTB_LINK_SPEED_MASK);
}

static inline enum ntb_width
ntb_link_sta_width(struct ntb_softc *ntb)
{

	if (!link_is_up(ntb))
		return (NTB_WIDTH_NONE);
	return (NTB_LNK_STA_WIDTH(ntb->lnk_sta));
}

/*
 * Public API to the rest of the OS
 */

/**
 * ntb_get_max_spads() - get the total scratch regs usable
 * @ntb: pointer to ntb_softc instance
 *
 * This function returns the max 32bit scratchpad registers usable by the
 * upper layer.
 *
 * RETURNS: total number of scratch pad registers available
 */
uint8_t
ntb_get_max_spads(struct ntb_softc *ntb)
{

	return (ntb->spad_count);
}

uint8_t
ntb_mw_count(struct ntb_softc *ntb)
{

	return (ntb->mw_count);
}

/**
 * ntb_spad_write() - write to the secondary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to the scratchpad register, 0 based
 * @val: the data value to put into the register
 *
 * This function allows writing of a 32bit value to the indexed scratchpad
 * register. The register resides on the secondary (external) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_spad_write(struct ntb_softc *ntb, unsigned int idx, uint32_t val)
{

	if (idx >= ntb->spad_count)
		return (EINVAL);

	ntb_reg_write(4, ntb->reg_ofs.spad_local + idx * 4, val);

	return (0);
}

/**
 * ntb_spad_read() - read from the primary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to scratchpad register, 0 based
 * @val: pointer to 32bit integer for storing the register value
 *
 * This function allows reading of the 32bit scratchpad register on
 * the primary (internal) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_spad_read(struct ntb_softc *ntb, unsigned int idx, uint32_t *val)
{

	if (idx >= ntb->spad_count)
		return (EINVAL);

	*val = ntb_reg_read(4, ntb->reg_ofs.spad_local + idx * 4);

	return (0);
}

/**
 * ntb_peer_spad_write() - write to the secondary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to the scratchpad register, 0 based
 * @val: the data value to put into the register
 *
 * This function allows writing of a 32bit value to the indexed scratchpad
 * register. The register resides on the secondary (external) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_peer_spad_write(struct ntb_softc *ntb, unsigned int idx, uint32_t val)
{

	if (idx >= ntb->spad_count)
		return (EINVAL);

	if (HAS_FEATURE(NTB_SDOORBELL_LOCKUP))
		ntb_mw_write(4, XEON_SHADOW_SPAD_OFFSET + idx * 4, val);
	else
		ntb_reg_write(4, ntb->peer_reg->spad + idx * 4, val);

	return (0);
}

/**
 * ntb_peer_spad_read() - read from the primary scratchpad register
 * @ntb: pointer to ntb_softc instance
 * @idx: index to scratchpad register, 0 based
 * @val: pointer to 32bit integer for storing the register value
 *
 * This function allows reading of the 32bit scratchpad register on
 * the primary (internal) side.
 *
 * RETURNS: An appropriate ERRNO error value on error, or zero for success.
 */
int
ntb_peer_spad_read(struct ntb_softc *ntb, unsigned int idx, uint32_t *val)
{

	if (idx >= ntb->spad_count)
		return (EINVAL);

	if (HAS_FEATURE(NTB_SDOORBELL_LOCKUP))
		*val = ntb_mw_read(4, XEON_SHADOW_SPAD_OFFSET + idx * 4);
	else
		*val = ntb_reg_read(4, ntb->peer_reg->spad + idx * 4);

	return (0);
}

/*
 * ntb_mw_get_range() - get the range of a memory window
 * @ntb:        NTB device context
 * @idx:        Memory window number
 * @base:       OUT - the base address for mapping the memory window
 * @size:       OUT - the size for mapping the memory window
 * @align:      OUT - the base alignment for translating the memory window
 * @align_size: OUT - the size alignment for translating the memory window
 *
 * Get the range of a memory window.  NULL may be given for any output
 * parameter if the value is not needed.  The base and size may be used for
 * mapping the memory window, to access the peer memory.  The alignment and
 * size may be used for translating the memory window, for the peer to access
 * memory on the local system.
 *
 * Return: Zero on success, otherwise an error number.
 */
int
ntb_mw_get_range(struct ntb_softc *ntb, unsigned mw_idx, vm_paddr_t *base,
    void **vbase, size_t *size, size_t *align, size_t *align_size)
{
	struct ntb_pci_bar_info *bar;
	size_t bar_b2b_off;

	if (mw_idx >= ntb_mw_count(ntb))
		return (EINVAL);

	bar = &ntb->bar_info[ntb_mw_to_bar(ntb, mw_idx)];
	bar_b2b_off = 0;
	if (mw_idx == ntb->b2b_mw_idx) {
		KASSERT(ntb->b2b_off != 0,
		    ("user shouldn't get non-shared b2b mw"));
		bar_b2b_off = ntb->b2b_off;
	}

	if (base != NULL)
		*base = bar->pbase + bar_b2b_off;
	if (vbase != NULL)
		*vbase = (char *)bar->vbase + bar_b2b_off;
	if (size != NULL)
		*size = bar->size - bar_b2b_off;
	if (align != NULL)
		*align = bar->size;
	if (align_size != NULL)
		*align_size = 1;
	return (0);
}

/*
 * ntb_mw_set_trans() - set the translation of a memory window
 * @ntb:        NTB device context
 * @idx:        Memory window number
 * @addr:       The dma address local memory to expose to the peer
 * @size:       The size of the local memory to expose to the peer
 *
 * Set the translation of a memory window.  The peer may access local memory
 * through the window starting at the address, up to the size.  The address
 * must be aligned to the alignment specified by ntb_mw_get_range().  The size
 * must be aligned to the size alignment specified by ntb_mw_get_range().
 *
 * Return: Zero on success, otherwise an error number.
 */
int
ntb_mw_set_trans(struct ntb_softc *ntb, unsigned idx, bus_addr_t addr,
    size_t size)
{
	struct ntb_pci_bar_info *bar;
	uint64_t base, limit, reg_val;
	size_t bar_size, mw_size;
	uint32_t base_reg, xlat_reg, limit_reg;
	enum ntb_bar bar_num;

	if (idx >= ntb_mw_count(ntb))
		return (EINVAL);

	bar_num = ntb_mw_to_bar(ntb, idx);
	bar = &ntb->bar_info[bar_num];

	bar_size = bar->size;
	if (idx == ntb->b2b_mw_idx)
		mw_size = bar_size - ntb->b2b_off;
	else
		mw_size = bar_size;

	/* Hardware requires that addr is aligned to bar size */
	if ((addr & (bar_size - 1)) != 0)
		return (EINVAL);

	if (size > mw_size)
		return (EINVAL);

	bar_get_xlat_params(ntb, bar_num, &base_reg, &xlat_reg, &limit_reg);

	limit = 0;
	if (bar_is_64bit(ntb, bar_num)) {
		base = ntb_reg_read(8, base_reg);

		if (limit_reg != 0 && size != mw_size)
			limit = base + size;

		/* Set and verify translation address */
		ntb_reg_write(8, xlat_reg, addr);
		reg_val = ntb_reg_read(8, xlat_reg);
		if (reg_val != addr) {
			ntb_reg_write(8, xlat_reg, 0);
			return (EIO);
		}

		/* Set and verify the limit */
		ntb_reg_write(8, limit_reg, limit);
		reg_val = ntb_reg_read(8, limit_reg);
		if (reg_val != limit) {
			ntb_reg_write(8, limit_reg, base);
			ntb_reg_write(8, xlat_reg, 0);
			return (EIO);
		}
	} else {
		/* Configure 32-bit (split) BAR MW */

		if ((addr & ~UINT32_MAX) != 0)
			return (EINVAL);
		if (((addr + size) & ~UINT32_MAX) != 0)
			return (EINVAL);

		base = ntb_reg_read(4, base_reg);

		if (limit_reg != 0 && size != mw_size)
			limit = base + size;

		/* Set and verify translation address */
		ntb_reg_write(4, xlat_reg, addr);
		reg_val = ntb_reg_read(4, xlat_reg);
		if (reg_val != addr) {
			ntb_reg_write(4, xlat_reg, 0);
			return (EIO);
		}

		/* Set and verify the limit */
		ntb_reg_write(4, limit_reg, limit);
		reg_val = ntb_reg_read(4, limit_reg);
		if (reg_val != limit) {
			ntb_reg_write(4, limit_reg, base);
			ntb_reg_write(4, xlat_reg, 0);
			return (EIO);
		}
	}
	return (0);
}

/*
 * ntb_mw_clear_trans() - clear the translation of a memory window
 * @ntb:	NTB device context
 * @idx:	Memory window number
 *
 * Clear the translation of a memory window.  The peer may no longer access
 * local memory through the window.
 *
 * Return: Zero on success, otherwise an error number.
 */
int
ntb_mw_clear_trans(struct ntb_softc *ntb, unsigned mw_idx)
{

	return (ntb_mw_set_trans(ntb, mw_idx, 0, 0));
}

/**
 * ntb_peer_db_set() - Set the doorbell on the secondary/external side
 * @ntb: pointer to ntb_softc instance
 * @bit: doorbell bits to ring
 *
 * This function allows triggering of a doorbell on the secondary/external
 * side that will initiate an interrupt on the remote host
 */
void
ntb_peer_db_set(struct ntb_softc *ntb, uint64_t bit)
{

	if (HAS_FEATURE(NTB_SDOORBELL_LOCKUP)) {
		ntb_mw_write(2, XEON_SHADOW_PDOORBELL_OFFSET, bit);
		return;
	}

	db_iowrite(ntb, ntb->peer_reg->db_bell, bit);
}

/*
 * ntb_get_peer_db_addr() - Return the address of the remote doorbell register,
 * as well as the size of the register (via *sz_out).
 *
 * This function allows a caller using I/OAT DMA to chain the remote doorbell
 * ring to its memory window write.
 *
 * Note that writing the peer doorbell via a memory window will *not* generate
 * an interrupt on the remote host; that must be done seperately.
 */
bus_addr_t
ntb_get_peer_db_addr(struct ntb_softc *ntb, vm_size_t *sz_out)
{
	struct ntb_pci_bar_info *bar;
	uint64_t regoff;

	KASSERT(sz_out != NULL, ("must be non-NULL"));

	if (!HAS_FEATURE(NTB_SDOORBELL_LOCKUP)) {
		bar = &ntb->bar_info[NTB_CONFIG_BAR];
		regoff = ntb->peer_reg->db_bell;
	} else {
		KASSERT((HAS_FEATURE(NTB_SPLIT_BAR) && ntb->mw_count == 2) ||
		    (!HAS_FEATURE(NTB_SPLIT_BAR) && ntb->mw_count == 1),
		    ("mw_count invalid after setup"));
		KASSERT(ntb->b2b_mw_idx != B2B_MW_DISABLED,
		    ("invalid b2b idx"));

		bar = &ntb->bar_info[ntb_mw_to_bar(ntb, ntb->b2b_mw_idx)];
		regoff = XEON_SHADOW_PDOORBELL_OFFSET;
	}
	KASSERT(bar->pci_bus_tag != X86_BUS_SPACE_IO, ("uh oh"));

	*sz_out = ntb->reg->db_size;
	/* HACK: Specific to current x86 bus implementation. */
	return ((uint64_t)bar->pci_bus_handle + regoff);
}

/*
 * ntb_db_valid_mask() - get a mask of doorbell bits supported by the ntb
 * @ntb:	NTB device context
 *
 * Hardware may support different number or arrangement of doorbell bits.
 *
 * Return: A mask of doorbell bits supported by the ntb.
 */
uint64_t
ntb_db_valid_mask(struct ntb_softc *ntb)
{

	return (ntb->db_valid_mask);
}

/*
 * ntb_db_vector_mask() - get a mask of doorbell bits serviced by a vector
 * @ntb:	NTB device context
 * @vector:	Doorbell vector number
 *
 * Each interrupt vector may have a different number or arrangement of bits.
 *
 * Return: A mask of doorbell bits serviced by a vector.
 */
uint64_t
ntb_db_vector_mask(struct ntb_softc *ntb, uint32_t vector)
{

	if (vector > ntb->db_vec_count)
		return (0);
	return (ntb->db_valid_mask & ntb_vec_mask(ntb, vector));
}

/**
 * ntb_link_is_up() - get the current ntb link state
 * @ntb:        NTB device context
 * @speed:      OUT - The link speed expressed as PCIe generation number
 * @width:      OUT - The link width expressed as the number of PCIe lanes
 *
 * RETURNS: true or false based on the hardware link state
 */
bool
ntb_link_is_up(struct ntb_softc *ntb, enum ntb_speed *speed,
    enum ntb_width *width)
{

	if (speed != NULL)
		*speed = ntb_link_sta_speed(ntb);
	if (width != NULL)
		*width = ntb_link_sta_width(ntb);
	return (link_is_up(ntb));
}

static void
save_bar_parameters(struct ntb_pci_bar_info *bar)
{

	bar->pci_bus_tag = rman_get_bustag(bar->pci_resource);
	bar->pci_bus_handle = rman_get_bushandle(bar->pci_resource);
	bar->pbase = rman_get_start(bar->pci_resource);
	bar->size = rman_get_size(bar->pci_resource);
	bar->vbase = rman_get_virtual(bar->pci_resource);
}

device_t
ntb_get_device(struct ntb_softc *ntb)
{

	return (ntb->device);
}

/* Export HW-specific errata information. */
bool
ntb_has_feature(struct ntb_softc *ntb, uint64_t feature)
{

	return (HAS_FEATURE(feature));
}
