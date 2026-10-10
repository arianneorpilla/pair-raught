/*
 * Wake timing. See puck_wake_timing.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include "puck_wake_timing.h"
#include "puck_settings.h"
#include "switch2_store.h"

#define WT_VERSION 2
#define WT_WRITES  16
#define WT_NONE    0xFFFFu

/*
 * After RF is back, wait this long for the controller to answer before saving
 * the record without it. A controller that was switched off during the wake
 * never answers, and the rest of the record is still worth having.
 */
#define WT_RF_WAIT_MS 5000

struct wake_record {
	uint8_t version;
	uint8_t count;
	uint16_t hold_ms;
	uint16_t ms[WT_COUNT];	/* after time zero, WT_NONE if not reached */
	uint8_t nwrites;
	uint8_t reserved;
	uint16_t w_handle[WT_WRITES];
	uint16_t w_ms[WT_WRITES];
};

static const char *const NAMES[WT_COUNT] = {
	[WT_BLE_UP]    = "advertising",
	[WT_CONNECTED] = "console connected",
	[WT_ENCRYPTED] = "link encrypted",
	[WT_STREAM]    = "report stream open",
	[WT_UNLOCK]    = "first unlock press",
	[WT_RELEASE]   = "released, to RF",
	[WT_RF_UP]     = "RF up",
	[WT_RF_LIVE]   = "controller answered",
};

static volatile bool g_on;
static uint32_t g_t0;
static uint16_t g_hold_ms;
static uint32_t g_at[WT_COUNT];
static bool g_have[WT_COUNT];
static uint8_t g_nwrites;
static uint16_t g_w_handle[WT_WRITES];
static uint32_t g_w_at[WT_WRITES];

static void print_record(const struct wake_record *r, const char *title)
{
	uint16_t prev = 0;

	printk("\n%s (ms after the QAM hold completed; the hold itself was "
	       "%u ms)\n", title, r->hold_ms);
	for (int i = 0; i < WT_COUNT && i < r->count; i++) {
		if (r->ms[i] == WT_NONE) {
			printk("  %s: --\n", NAMES[i]);
			continue;
		}
		printk("  %s: %u ms (+%u)\n", NAMES[i], r->ms[i],
		       r->ms[i] >= prev ? r->ms[i] - prev : 0);
		prev = r->ms[i];
	}
	for (int i = 0; i < r->nwrites && i < WT_WRITES; i++) {
		printk("  console wrote 0x%04X: %u ms\n", r->w_handle[i],
		       r->w_ms[i]);
	}
}

static void save_fn(struct k_work *work)
{
	struct wake_record r = {
		.version = WT_VERSION,
		.count = WT_COUNT,
	};

	ARG_UNUSED(work);

	if (!g_on) {
		return;
	}
	g_on = false;

	r.hold_ms = g_hold_ms;
	for (int i = 0; i < WT_COUNT; i++) {
		uint32_t d = g_at[i] - g_t0;

		r.ms[i] = g_have[i] ? (uint16_t)MIN(d, WT_NONE - 1) : WT_NONE;
	}
	r.nwrites = g_nwrites;
	for (int i = 0; i < g_nwrites; i++) {
		r.w_handle[i] = g_w_handle[i];
		r.w_ms[i] = (uint16_t)MIN(g_w_at[i] - g_t0, WT_NONE - 1);
	}

	print_record(&r, "wake timing");
	if (switch2_store_save_wake_timing(&r, sizeof(r))) {
		printk("  (not saved: storage unavailable)\n");
	}
}

static K_WORK_DELAYABLE_DEFINE(g_save_work, save_fn);

void wake_timing_start(void)
{
	memset(g_have, 0, sizeof(g_have));
	g_nwrites = 0;
	g_hold_ms = g_cfg.wake_hold_ms;
	g_t0 = k_uptime_get_32();
	g_on = true;
}

void wake_timing_mark(enum wake_mark m)
{
	if (!g_on || m >= WT_COUNT || g_have[m]) {
		return;
	}
	/* RF is still running while the wake chime plays, so a reply only
	 * counts once the radio has come back from BLE.
	 */
	if (m == WT_RF_LIVE && !g_have[WT_RF_UP]) {
		return;
	}
	g_at[m] = k_uptime_get_32();
	g_have[m] = true;

	if (m == WT_RF_UP) {
		k_work_reschedule(&g_save_work, K_MSEC(WT_RF_WAIT_MS));
	} else if (m == WT_RF_LIVE) {
		k_work_reschedule(&g_save_work, K_NO_WAIT);
	}
}

/* ---- Boot records ------------------------------------------------------ */

#define BR_BOOTS  3	/* this boot and the two before it */
#define BR_EVENTS 12	/* the most recent events of each boot */

struct boot_rec {
	uint32_t boot_no;	/* 0 = unused */
	uint16_t total;		/* events seen, including any overwritten */
	uint8_t type[BR_EVENTS];
	uint32_t ms[BR_EVENTS];	/* uptime when it happened */
};

static struct boot_rec g_boots[BR_BOOTS];	/* [0] is this boot */
static bool g_boots_ready;

static void boot_save_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	switch2_store_save_boot_records(g_boots, sizeof(g_boots));
}

static K_WORK_DELAYABLE_DEFINE(g_boot_save_work, boot_save_fn);

void boot_record_init(void)
{
	if (!switch2_store_load_boot_records(g_boots, sizeof(g_boots))) {
		memset(g_boots, 0, sizeof(g_boots));
	}
	memmove(&g_boots[1], &g_boots[0], sizeof(g_boots[0]) * (BR_BOOTS - 1));
	memset(&g_boots[0], 0, sizeof(g_boots[0]));
	g_boots[0].boot_no = g_boots[1].boot_no + 1;
	g_boots_ready = true;
	switch2_store_save_boot_records(g_boots, sizeof(g_boots));
}

void boot_record_event(uint8_t type)
{
	struct boot_rec *b = &g_boots[0];
	uint16_t i;

	if (!g_boots_ready) {
		return;
	}
	i = b->total % BR_EVENTS;
	b->type[i] = type;
	b->ms[i] = k_uptime_get_32();
	b->total++;

	/* Batched: a bus reset, a configuration and a resume arrive together. */
	k_work_reschedule(&g_boot_save_work, K_MSEC(1000));
}

static const char *ev_name(uint8_t t)
{
	static const char *const usb[] = {
		"VBUS ready", "VBUS removed", "USB resumed", "USB suspended",
		"USB bus reset", "USB configured",
	};

	if (t < ARRAY_SIZE(usb)) {
		return usb[t];
	}
	if (t == BOOT_EV_RF_UP) {
		return "radio to RF";
	}
	if (t == BOOT_EV_BLE_UP) {
		return "radio to BLE";
	}
	return "other USB event";
}

static void boot_records_dump(void)
{
	for (int n = 0; n < BR_BOOTS; n++) {
		const struct boot_rec *b = &g_boots[n];
		uint16_t shown = MIN(b->total, BR_EVENTS);
		uint16_t first = b->total - shown;

		if (!b->boot_no) {
			continue;
		}
		printk("\nboot %u%s: %u event(s)%s\n", b->boot_no,
		       n == 0 ? " (this boot)" : "", b->total,
		       b->total > BR_EVENTS ? ", oldest dropped" : "");
		for (uint16_t k = first; k < b->total; k++) {
			uint16_t i = k % BR_EVENTS;

			printk("  %u ms: %s\n", b->ms[i], ev_name(b->type[i]));
		}
	}
}

void wake_timing_write(uint16_t handle)
{
	/* Only during a wake, and only once the console is on the link. */
	if (!g_on || !g_have[WT_CONNECTED] || g_have[WT_RELEASE] ||
	    g_nwrites >= WT_WRITES) {
		return;
	}
	g_w_handle[g_nwrites] = handle;
	g_w_at[g_nwrites] = k_uptime_get_32();
	g_nwrites++;
}

void wake_timing_dump(void)
{
	struct wake_record r;

	if (!switch2_store_load_wake_timing(&r, sizeof(r)) ||
	    r.version != WT_VERSION) {
		printk("\nno wake timing recorded yet\n");
	} else {
		print_record(&r, "last wake");
	}
	boot_records_dump();
}
