/*
 * Sensor collection framework header
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

#ifndef	SENSCOL_CORE__
#define	SENSCOL_CORE__

#define SENSCOL_1	1 /* 1: senscol_1, 0: senscol_2*/

#include <linux/types.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#if SENSCOL_1
#include <linux/kobject.h>
#endif

#define	PSEUSO_EVENT_BIT	(1<<31)
#define	FLUSH_CMPL_BIT		(1<<0)

struct data_field;
struct sens_property;
struct senscol_impl;

struct sensor_def {
	char	*name;			/* Must be not NULL */
	uint32_t	usage_id;	/* Usage ID */
	char	*friendly_name;		/* May be NULL */
	int	num_data_fields;	/* Size of array of data fields */
	struct data_field *data_fields;	/* Array of data fields */
	int	num_properties;		/* Size of array of properties */
	struct sens_property *properties;	/* Array of properties*/
	uint32_t	id;		/* Unique ID of sensor
						(running count of
						discovery iteration)*/
	int	sample_size;		/* Derived from array of data_fields,
						updated when every
						data_field is added */
	struct senscol_impl	*impl;
	struct using_fd *acquire_fd;	/* fd acquired the sensor,
						NULL if sensor is available */
	int ask_flush_count;

	struct list_head	link;

	wait_queue_head_t wait_sens_free;
	int sens_in_use;		/* use the generic senscol_lock to
						protect	the "in-use" flag of
						each sensor */
#if SENSCOL_1
	struct kobject	kobj;
	struct kobject	data_fields_kobj;
	struct kobject	props_kobj;
	int	flush_req;
#endif
};

struct data_field {
	char	*name;			/* Must be not NULL */
	uint32_t	usage_id;	/* Usage ID of data_field */
	uint8_t	exp;			/* Exponent: 0..F */
	uint8_t	len;			/* Length: 0..4 */
	uint32_t	unit;		/* Usage ID of unit */
	int	is_numeric;
	struct sensor_def	*sensor;/* We need backlink for properties to
					 * their parent sensors */
	int	index;			/* Index of field in raw data */
#if SENSCOL_1
	struct kobject	kobj;
#endif
};

struct sens_property {
	char *name;			/* Must be not NULL */
	uint32_t	usage_id;	/* Usage ID of sens_property */
	char	*value;
	uint32_t	unit;		/* Usage ID of unit */
	int	is_string;		/* If !is_numeric,
					 * only name and usage_id appear */
	struct sensor_def	*sensor;/* We need backlink for properties
					 * to their parent sensors */
	struct list_head	link;
#if SENSCOL_1
	struct kobject	kobj;
#endif
};

/* fd using the sensors details */
struct using_fd {
	uint8_t *data_buf;
	unsigned head;
	unsigned tail;
	spinlock_t buf_lock;
	wait_queue_head_t read_wait;
	struct list_head sensors_list;
	int comapt_ioctl;	/* the flag is set in ioctl func
				 * if it was called from the compat_ioctl func*/
	struct task_struct *user_task; /* task to send reset notify event*/

	wait_queue_head_t wait_fd_free;
#define FD_BEING_REMOVED	(-1)
	int fd_used_cnt; /*	-1: in remove process
				0: free
				>0: cnt of using (either push_sample and/or
					flush callback)
				use the generic fds_list_lock to protect it */
	struct list_head link;
};

/* pointer to sensor to save in sensors list of using_fd struct */
struct sens_link {
	struct sensor_def *sensor;
	struct list_head link;
};

/* Sample structure */
struct senscol_sample {
	uint32_t	id;
	uint32_t	size;	/* For easier/faster
				 * traversing of FIFO during reads */
	uint8_t	data[1];	/* `size' (one or more) bytes of data */
} __packed;

/*
 * Samples are queued is a simple FIFO binary buffer
 * with head and tail pointers.
 * If the buffer wraps around, a single sample not start past
 * SENSCOL_DATA_BUF_LAST, but may cross it or start at it
 * Additional fields if wanted to be communicated to user mode can be define
 */

/*
 * Suggested size of data buffer:
 * avg 24 bytes per sampl
 * expected 2600 samples/s for 17 sensors at max. rate
 * cover for 10 seconds of data
 */

#define	SENSCOL_DATA_BUF_SIZE	(0x40000)
#define MAX_SENS_SAMPLE_SIZE	128
#define	SENSCOL_DATA_BUF_LAST	(SENSCOL_DATA_BUF_SIZE - MAX_SENS_SAMPLE_SIZE)

/*
 * Sensor collection underlying handler.
 * Supplies set_prop(), get_prop() and get_sample() callback
 */
struct senscol_impl {
#if SENSCOL_1
	/* Get property value, will return NULL on failure */
	int	(*get_sens_property)(struct sensor_def *sensor,
		const struct sens_property *prop, char *value,
		size_t val_buf_size);

	/* Set property value */
	int	(*set_sens_property)(struct sensor_def *sensor,
		const struct sens_property *prop, const char *value);
#endif

	/* Get sample */
	int	(*get_sample)(struct sensor_def *sensor);

	/* Check if sensor is activated in batch mode */
	int	(*disable_sensor)(struct sensor_def *sensor);

	int	(*get_sens_properties)(struct sensor_def *sensor);
	int	(*set_sens_properties)(struct list_head *set_prop_list,
		uint32_t sensor_id);
	int	(*ask_flush)(struct sensor_def *sensor);

	struct list_head link;
};

/* structs for API get-set using ioctl */
#define MAX_NAME_LENGTH		56
#define MAX_PROP_NAME_LEN	80
#define MAX_PROP_VAL_LEN	56
#define MAX_DATA_FIELD_NAME_LEN	80

struct ioctl_res;

#define IOCTL_GET_SENSORS_NUM	_IOWR('H', 0x01, int)
#define IOCTL_GET_SENSORS_LIST	_IOWR('H', 0x02, struct ioctl_res)
#define IOCTL_GET_PROP		_IOWR('H', 0x03, struct ioctl_res)
#define IOCTL_SET_PROP		_IOWR('H', 0x04, struct ioctl_res)
#define IOCTL_GET_DATA_FIELDS	_IOWR('H', 0x05, struct ioctl_res)
#define IOCTL_FLUSH		_IOWR('H', 0x06, int)
#define IOCTL_GET_SAMPLE	_IOWR('H', 0x07, int)

struct ioctl_res {
	uint32_t buffer_size;	/* user buffer size */
	uint32_t sens_id;	/* sensor id, 0 in general cmd */
	uint32_t response_size;	/* size of response from driver */
	uint64_t buffer_ptr;	/* user buffer */
} __packed;

struct sensor_details {
	uint32_t id;
	char name[MAX_NAME_LENGTH];
	uint32_t usage_id;
	uint32_t sample_size;
	uint32_t properties_num;	/* number of properties */
	uint32_t data_fields_num;	/* number of data fields */
} __packed;

struct property_details {
	char prop_name[MAX_PROP_NAME_LEN];
	char value[MAX_PROP_VAL_LEN];
} __packed;

struct data_fields_details {
	char data_field_name[MAX_DATA_FIELD_NAME_LEN];
	uint32_t usage_id;
	uint32_t index;
	uint32_t len;
	uint8_t is_numeric;
	uint32_t exponent;
	uint32_t unit;
} __packed;

/***************************************/

#if SENSCOL_1
/* init and exit functions of sensor-collection */
int	senscol_init_legacy(void);
void	senscol_exit_legacy(void);

/* Init sensor (don't call for initialized sensors) */
void	init_senscol_sensor_legacy(struct sensor_def *sensor);

int	add_senscol_sensor_legacy(struct sensor_def *sensor);
int	remove_senscol_sensor_legacy(uint32_t id);

/*
 * Push data sample in upstream buffer towards user-mode.
 * Sample's size is determined from the structure
 */
int	push_sample_legacy(uint32_t id, void *sample);
void	senscol_flush_cb_legacy(unsigned sens_id);

void	senscol_send_ready_event_legacy(void);
int	senscol_reset_notify_legacy(void);

#else /* senscol2 api */

/* init and exit functions of sensor-collection */
int	senscol_init(void);
void	senscol_exit(void);

/* Init sensor (don't call for initialized sensors) */
void	init_senscol_sensor(struct sensor_def *sensor);

int	add_senscol_sensor(struct sensor_def *sensor);
int	remove_senscol_sensor(uint32_t id);

/*
 * Push data sample in upstream buffer towards user-mode.
 * Sample's size is determined from the structure
 */
int	push_sample(uint32_t id, void *sample);
void	senscol_flush_cb(unsigned sens_id);

void	senscol_send_ready_event(void);
int	senscol_reset_notify(void);
#endif /* SENSCOL_1 */

/* Only allocates new sensor */
struct sensor_def *alloc_senscol_sensor(void);
/* Add data field to existing sensor */
int	add_data_field(struct sensor_def *sensor, struct data_field *data);
/* Add property to existing sensor */
int	add_sens_property(struct sensor_def *sensor,
	struct sens_property *prop);

struct sensor_def	*get_senscol_sensor_by_id(uint32_t id);
/* Returns known name of given usages (NULL if unknown) */
const char	*senscol_usage_to_name(unsigned usage);
/* Returns known name of given modifier safe, always returns value*/
const char	*senscol_get_modifier(unsigned modif);

/* Allocates string with the name of the given usage */
char	*create_sensor_name(unsigned usage_id);
char	*create_sens_prop_name(unsigned usage_id);

extern struct list_head	senscol_sensors_list;
extern spinlock_t	senscol_lock;

/*********** Locally redirect ish_print_log **************/
void g_ish_print_log(char *format, ...);
/*********************************************************/
#endif /*SENSCOL_CORE__*/

