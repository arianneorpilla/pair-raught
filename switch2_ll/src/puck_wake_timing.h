/*
 * Wake timing: where the time goes between a QAM hold and the controller
 * answering on RF again.
 *
 * Each step of a wake is stamped with k_uptime_get_32() as it happens. The
 * stamps cost nothing at the call site, so they can sit on the radio paths.
 * Printing and saving happen later on the system workqueue.
 *
 * The record is saved to NVS, because a puck plugged into the console has no
 * terminal. Plug it back into a PC and press 'l' on the console, or read the
 * copy printed at boot.
 */

#ifndef PUCK_WAKE_TIMING_H
#define PUCK_WAKE_TIMING_H

#include <stdint.h>

/* Ordered as a wake normally runs. The values are the record's layout. */
enum wake_mark {
	WT_BLE_UP,	/* advertising with the wake byte set */
	WT_CONNECTED,	/* the console opened a link */
	WT_ENCRYPTED,	/* the link is encrypted */
	WT_STREAM,	/* the report stream opened */
	WT_UNLOCK,	/* first unlock press goes out */
	WT_RELEASE,	/* unlock done, handover to RF requested */
	WT_RF_UP,	/* the radio belongs to RF again */
	WT_RF_LIVE,	/* the controller answered a poll */
	WT_COUNT
};

/* QAM hold has completed and the wake is accepted. Stamps time zero. */
void wake_timing_start(void);

/* Stamp a step. Only the first stamp of each step in a wake is kept. */
void wake_timing_mark(enum wake_mark m);

/*
 * The console wrote to this attribute (an ATT write or a CCCD change). The
 * first few of each wake are kept with their times, to find what the console
 * sends when its lock screen is up, so the unlock can start on that rather
 * than on a fixed delay.
 */
void wake_timing_write(uint16_t handle);

/* Print the last saved record, and the boot records below. */
void wake_timing_dump(void);

/*
 * Boot records: what USB and the radio did during each of the last three
 * boots, saved to NVS, so a power cut shows up as a boot of its own.
 *
 * A console that cuts dock power when it goes to sleep leaves a boot that
 * starts with no "configured" event, because the console is asleep and never
 * enumerates the puck. A console that keeps power on leaves a "suspended"
 * event and no new boot.
 */
#define BOOT_EV_RF_UP  0xF0	/* the radio went to RF */
#define BOOT_EV_BLE_UP 0xF1	/* the radio went to BLE */

/* Once per boot, after storage is mounted. */
void boot_record_init(void);

/* A usbd_msg_type, or one of BOOT_EV_*. Call from thread context. */
void boot_record_event(uint8_t type);

#endif /* PUCK_WAKE_TIMING_H */
