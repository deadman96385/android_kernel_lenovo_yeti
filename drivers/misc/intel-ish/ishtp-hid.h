/*
 * ISHTP-HID glue driver's definitions.
 *
 * Copyright (c) 2014-2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 */
#ifndef ISHTP_HID__H
#define	ISHTP_HID__H

/*
 * Currently we support only 1 ISH HW controller in a system
 */

/*
 * TODO: figure out if this number is used for anything but assignment.
 * (http://www.lanana.org)
 */
#define	BUS_ISHTP	0x44
/*
 * TODO: just to bootstrap, numbers will probably change
 * (http://www.lanana.org)
 */
#define	ISH_HID_VENDOR	0x8086
#define	ISH_HID_PRODUCT	0x22D8
#define	ISH_HID_VERSION	0x0200

#define	CMD_MASK	0x7F
#define	IS_RESPONSE	0x80

static const uuid_le hid_ishtp_guid = UUID_LE(0x33AECD58, 0xB679, 0x4E54,
						0x9B, 0xD9, 0xA0, 0x4D, 0x34,
						0xF0, 0xC2, 0x26);

extern wait_queue_head_t	ishtp_hid_wait;

/* flush notification */
extern void (*flush_cb)(void);

struct hostif_msg_hdr {
	uint8_t	command;	/* Bit 7: is_response */
	uint8_t	device_id;
	uint8_t	status;
	uint8_t	flags;
	uint16_t	size;
} __packed;

struct hostif_msg {
	struct hostif_msg_hdr	hdr;
} __packed;

struct hostif_msg_to_sensor {
	struct hostif_msg_hdr	hdr;
	uint8_t	report_id;
} __packed;

struct device_info {
	uint32_t dev_id;
	uint8_t dev_class;
	uint16_t pid;
	uint16_t vid;
} __packed;

struct ishtp_version {
	uint8_t	major;
	uint8_t	minor;
	uint8_t	hotfix;
	uint16_t build;
} __packed;

/*
 * struct for ishtp aggregated input data
 */
struct report_list {
	uint16_t total_size;
	uint8_t	num_of_reports;
	uint8_t	flags;
	struct {
		uint16_t	size_of_report;
		uint8_t report[1];
	} __packed reports[1];
} __packed;

/* HOSTIF commands */
#define	HOSTIF_HID_COMMAND_BASE		0
#define	HOSTIF_GET_HID_DESCRIPTOR	0
#define	HOSTIF_GET_REPORT_DESCRIPTOR	1
#define HOSTIF_GET_FEATURE_REPORT	2
#define	HOSTIF_SET_FEATURE_REPORT	3
#define	HOSTIF_GET_INPUT_REPORT		4
#define	HOSTIF_PUBLISH_INPUT_REPORT	5
#define	HOSTIF_PUBLISH_INPUT_REPORT_LIST	6
#define	HOSTIF_DM_COMMAND_BASE		32
#define	HOSTIF_DM_ENUM_DEVICES		33
#define	HOSTIF_DM_ADD_DEVICE		34

#define	MAX_HID_DEVICES	32

extern unsigned char	*report_descr[MAX_HID_DEVICES];
extern int	report_descr_size[MAX_HID_DEVICES];
extern struct device_info	*hid_devices;
extern int	get_report_done; /* Get Feature/Input report complete flag */
extern unsigned	cur_hid_dev;
extern struct hid_device	*hid_sensor_hubs[MAX_HID_DEVICES];
extern unsigned	num_hid_devices;
extern struct ishtp_cl	*hid_ishtp_cl;	/* ISHTP client */

void hid_ishtp_set_feature(struct hid_device *hid, char *buf, unsigned len,
	int report_id);
void hid_ishtp_get_report(struct hid_device *hid, int report_id,
	int report_type);

int	ishtp_hid_probe(unsigned cur_hid_dev);
void	ishtp_hid_remove(void);

/* flush notification */
void register_flush_cb(void (*flush_cb_func)(void));

/*********** Locally redirect ish_print_log **************/
void g_ish_print_log(char *format, ...);
/*********************************************************/

#endif	/* ISHTP_HID__H */

