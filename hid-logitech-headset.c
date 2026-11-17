// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HID driver for the battery of Logitech wireless gaming headsets that report
 * it over a vendor collection instead of HID++.
 *
 * Copyright (c) 2026 Méven Car <meven@kde.org>
 */

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

/* hid-ids.h is private to drivers/hid, so an out-of-tree build carries the ids. */
#ifndef USB_VENDOR_ID_LOGITECH
#define USB_VENDOR_ID_LOGITECH				0x046d
#endif
#ifndef USB_DEVICE_ID_LOGITECH_PRO_X_2_LIGHTSPEED
#define USB_DEVICE_ID_LOGITECH_PRO_X_2_LIGHTSPEED	0x0af7
#endif
#ifndef USB_DEVICE_ID_LOGITECH_G325_LIGHTSPEED
#define USB_DEVICE_ID_LOGITECH_G325_LIGHTSPEED		0x0b21
#endif

/*
 * Both vendor collections exchange fixed size frames, and a request is a single
 * frame with no handshake. What differs between the headsets is the report
 * they use and the layout of the frames, so each one has a logi_headset_info.
 */
#define LOGI_HEADSET_FRAME_SIZE		64

#define LOGI_HEADSET_POLL_INTERVAL	(120 * HZ)

/* Wait between an event from the headset and the request it leads to. */
#define LOGI_HEADSET_WAKE_DELAY		HZ

/*
 * The PRO X 2 receiver carries a consumer collection for the media keys and
 * two vendor collections. The battery answers on usage page 0xffa0, on report
 * 0x51.
 */
#define LOGI_PRO_X_2_REPORT_ID		0x51

/* Every frame names its kind in the second byte. */
#define LOGI_PRO_X_2_KIND_ACK		0x03
#define LOGI_PRO_X_2_KIND_POWER		0x05
#define LOGI_PRO_X_2_KIND_BATTERY	0x0b

/* Offsets into a battery frame, which carries a tag of its own. */
#define LOGI_PRO_X_2_BATTERY_TAG	8
#define LOGI_PRO_X_2_BATTERY_TAG_VALUE	0x04
#define LOGI_PRO_X_2_BATTERY_LEVEL	10
#define LOGI_PRO_X_2_BATTERY_STATE	12
#define LOGI_PRO_X_2_STATE_CHARGING	0x02

/*
 * A power frame carries zero here when the headset was switched off, and
 * non-zero when it came on.
 */
#define LOGI_PRO_X_2_POWER_STATE	6

static const u8 logi_pro_x_2_battery_request[] = {
	LOGI_PRO_X_2_REPORT_ID, 0x08, 0x00, 0x03, 0x1a, 0x00, 0x03, 0x00, 0x04, 0x0a
};

/*
 * The G325 LIGHTSPEED receiver has the same collections, with the battery on
 * usage page 0xffa0, report 0x50. Its frames carry 0x23 in the second byte and
 * name their kind in the third:
 *
 *	ack:     50 23 03 00 03 1a 00 00 ...
 *	battery: 50 23 0b 00 03 10 00 06 00 07 0a XX LL SS ...
 *	event:   50 23 0b 00 03 10 00 06 00 07 00 XX LL SS ...
 *	power:   50 23 05 00 03 00 00 PP ...
 *
 * LL is the level in percent and SS the charging state. XX moves between two
 * readings and nothing says what it is, so it is not used. PP is 0 when the
 * headset is switched off and 1 when it comes on, and each change comes with a
 * second frame, 50 23 04 00 05 10 PP, that is not used either.
 *
 * The event frame comes unasked, each time the charger is plugged or
 * unplugged. It has the layout of a battery frame, but nothing says what else
 * it announces, so it only makes the driver ask again rather than be read.
 */
#define LOGI_G325_REPORT_ID		0x50

#define LOGI_G325_HEADER		1
#define LOGI_G325_HEADER_VALUE		0x23

#define LOGI_G325_KIND			2
#define LOGI_G325_KIND_ACK		0x03
#define LOGI_G325_KIND_POWER		0x05
#define LOGI_G325_KIND_BATTERY		0x0b

#define LOGI_G325_POWER_STATE		7
#define LOGI_G325_POWER_OFF		0x00
#define LOGI_G325_POWER_ON		0x01

/* A battery frame carries the same two bytes as the request that asked for it. */
#define LOGI_G325_BATTERY_TAG		9
#define LOGI_G325_BATTERY_LEVEL		12
#define LOGI_G325_BATTERY_STATE		13
#define LOGI_G325_STATE_DISCHARGING	0x00
#define LOGI_G325_STATE_CHARGING	0x02

/* The reply came within 70 ms on the headset this was written on. */
#define LOGI_G325_REPLY_TIMEOUT		(3 * HZ)

static const u8 logi_g325_battery_request[] = {
	LOGI_G325_REPORT_ID, 0x23, 0x0b, 0x00, 0x03, 0x1a, 0x00, 0x03, 0x00, 0x07, 0x0a
};

static const u8 logi_g325_battery_tag[] = { 0x07, 0x0a };
static const u8 logi_g325_event_tag[] = { 0x07, 0x00 };

/* What a frame turned out to be, once the headset specific parser read it. */
enum logi_headset_frame {
	LOGI_HEADSET_FRAME_IGNORE,	/* not about the battery or the power */
	LOGI_HEADSET_FRAME_ACK,		/* the request arrived, no reading yet */
	LOGI_HEADSET_FRAME_INVALID,	/* a battery frame whose values make no sense */
	LOGI_HEADSET_FRAME_BATTERY,	/* the reading holds a level and a state */
	LOGI_HEADSET_FRAME_CHANGED,	/* the battery changed, ask for a reading */
	LOGI_HEADSET_FRAME_POWER_ON,
	LOGI_HEADSET_FRAME_POWER_OFF,
};

struct logi_headset_reading {
	int capacity;
	bool charging;
};

struct logi_headset_info {
	u8 report_id;
	const u8 *request;
	size_t request_size;
	/*
	 * How long to wait for a reading after a request before reporting no
	 * data, or zero to wait for an event. A headset that drops out of
	 * range or runs flat may not send one.
	 */
	unsigned long reply_timeout;
	enum logi_headset_frame (*parse)(const u8 *data, int size,
					 struct logi_headset_reading *reading);
};

struct logi_headset {
	struct hid_device *hdev;
	const struct logi_headset_info *info;
	struct power_supply *battery;
	struct power_supply_desc desc;
	struct delayed_work poll;
	struct delayed_work timeout;
	spinlock_t lock;	/* guards the values below */
	int capacity;
	bool charging;
	bool present;		/* capacity and charging hold a reading from the headset */
	bool awaiting;		/* a request was sent and no reading came back yet */
	bool acked;		/* the request that is awaited was acknowledged */
	bool wake_queued;	/* an event moved the next request forward */
	bool removing;		/* events no longer queue a request */
};

static enum power_supply_property logi_headset_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
};

static int logi_headset_get_property(struct power_supply *psy,
				     enum power_supply_property prop,
				     union power_supply_propval *val)
{
	struct logi_headset *headset = power_supply_get_drvdata(psy);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&headset->lock, flags);
	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = headset->present;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!headset->present)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (headset->charging)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (headset->present)
			val->intval = headset->capacity;
		else
			ret = -ENODATA;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (!headset->present)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		else if (headset->capacity <= 10)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		else if (headset->capacity <= 25)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		else if (headset->capacity >= 95)
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		else
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		/* A peripheral, not the battery of the machine itself. */
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = headset->hdev->name;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Logitech";
		break;
	default:
		ret = -EINVAL;
		break;
	}
	spin_unlock_irqrestore(&headset->lock, flags);

	return ret;
}

static int logi_headset_ask(struct logi_headset *headset)
{
	const struct logi_headset_info *info = headset->info;
	u8 *frame;
	int ret;

	frame = kzalloc(LOGI_HEADSET_FRAME_SIZE, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	memcpy(frame, info->request, info->request_size);

	/* Same fallback as hidraw, for a device without an interrupt out endpoint. */
	ret = hid_hw_output_report(headset->hdev, frame, LOGI_HEADSET_FRAME_SIZE);
	if (ret == -ENOSYS)
		ret = hid_hw_raw_request(headset->hdev, frame[0], frame,
					 LOGI_HEADSET_FRAME_SIZE,
					 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	kfree(frame);

	return ret;
}

static void logi_headset_poll(struct work_struct *work)
{
	struct logi_headset *headset = container_of(to_delayed_work(work),
						    struct logi_headset, poll);
	unsigned long reply_timeout = headset->info->reply_timeout;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&headset->lock, flags);
	headset->wake_queued = false;
	headset->awaiting = reply_timeout != 0;
	headset->acked = false;
	spin_unlock_irqrestore(&headset->lock, flags);

	ret = logi_headset_ask(headset);
	if (ret < 0)
		hid_dbg(headset->hdev, "battery request failed: %d\n", ret);

	/* Also covers a request that never left: no reading will come for it. */
	if (reply_timeout)
		mod_delayed_work(system_dfl_wq, &headset->timeout, reply_timeout);

	queue_delayed_work(system_dfl_wq, &headset->poll, LOGI_HEADSET_POLL_INTERVAL);
}

/* A request got no reading in time, so what is held is stale: report no data. */
static void logi_headset_timeout(struct work_struct *work)
{
	struct logi_headset *headset = container_of(to_delayed_work(work),
						    struct logi_headset, timeout);
	bool missed = false, acked = false, changed = false;
	unsigned long flags;

	spin_lock_irqsave(&headset->lock, flags);
	if (headset->awaiting) {
		missed = true;
		acked = headset->acked;
		headset->awaiting = false;
		changed = headset->present;
		headset->present = false;
	}
	spin_unlock_irqrestore(&headset->lock, flags);

	if (missed)
		hid_dbg(headset->hdev, "no battery reading, request %sacknowledged\n",
			acked ? "" : "not ");

	if (changed)
		power_supply_changed(headset->battery);
}

static enum logi_headset_frame
logi_pro_x_2_parse(const u8 *data, int size, struct logi_headset_reading *reading)
{
	if (size <= LOGI_PRO_X_2_BATTERY_STATE)
		return LOGI_HEADSET_FRAME_IGNORE;

	switch (data[1]) {
	case LOGI_PRO_X_2_KIND_BATTERY:
		if (data[LOGI_PRO_X_2_BATTERY_TAG] != LOGI_PRO_X_2_BATTERY_TAG_VALUE ||
		    data[LOGI_PRO_X_2_BATTERY_LEVEL] > 100)
			return LOGI_HEADSET_FRAME_IGNORE;

		reading->capacity = data[LOGI_PRO_X_2_BATTERY_LEVEL];
		reading->charging = data[LOGI_PRO_X_2_BATTERY_STATE] ==
				    LOGI_PRO_X_2_STATE_CHARGING;
		return LOGI_HEADSET_FRAME_BATTERY;
	case LOGI_PRO_X_2_KIND_POWER:
		if (data[LOGI_PRO_X_2_POWER_STATE] == 0x00)
			return LOGI_HEADSET_FRAME_POWER_OFF;
		return LOGI_HEADSET_FRAME_POWER_ON;
	case LOGI_PRO_X_2_KIND_ACK:
		return LOGI_HEADSET_FRAME_ACK;
	default:
		return LOGI_HEADSET_FRAME_IGNORE;
	}
}

static const struct logi_headset_info logi_pro_x_2_info = {
	.report_id = LOGI_PRO_X_2_REPORT_ID,
	.request = logi_pro_x_2_battery_request,
	.request_size = sizeof(logi_pro_x_2_battery_request),
	.parse = logi_pro_x_2_parse,
};

static enum logi_headset_frame
logi_g325_parse(const u8 *data, int size, struct logi_headset_reading *reading)
{
	if (size <= LOGI_G325_BATTERY_STATE ||
	    data[LOGI_G325_HEADER] != LOGI_G325_HEADER_VALUE)
		return LOGI_HEADSET_FRAME_IGNORE;

	switch (data[LOGI_G325_KIND]) {
	case LOGI_G325_KIND_ACK:
		return LOGI_HEADSET_FRAME_ACK;
	case LOGI_G325_KIND_POWER:
		switch (data[LOGI_G325_POWER_STATE]) {
		case LOGI_G325_POWER_OFF:
			return LOGI_HEADSET_FRAME_POWER_OFF;
		case LOGI_G325_POWER_ON:
			return LOGI_HEADSET_FRAME_POWER_ON;
		default:
			return LOGI_HEADSET_FRAME_IGNORE;
		}
	case LOGI_G325_KIND_BATTERY:
		if (!memcmp(&data[LOGI_G325_BATTERY_TAG], logi_g325_event_tag,
			    sizeof(logi_g325_event_tag)))
			return LOGI_HEADSET_FRAME_CHANGED;

		if (memcmp(&data[LOGI_G325_BATTERY_TAG], logi_g325_battery_tag,
			   sizeof(logi_g325_battery_tag)))
			return LOGI_HEADSET_FRAME_IGNORE;

		/* A level above 100 or a state not seen so far is not a reading to trust. */
		if (data[LOGI_G325_BATTERY_LEVEL] > 100)
			return LOGI_HEADSET_FRAME_INVALID;

		switch (data[LOGI_G325_BATTERY_STATE]) {
		case LOGI_G325_STATE_DISCHARGING:
			reading->charging = false;
			break;
		case LOGI_G325_STATE_CHARGING:
			reading->charging = true;
			break;
		default:
			return LOGI_HEADSET_FRAME_INVALID;
		}

		reading->capacity = data[LOGI_G325_BATTERY_LEVEL];
		return LOGI_HEADSET_FRAME_BATTERY;
	default:
		return LOGI_HEADSET_FRAME_IGNORE;
	}
}

static const struct logi_headset_info logi_g325_info = {
	.report_id = LOGI_G325_REPORT_ID,
	.request = logi_g325_battery_request,
	.request_size = sizeof(logi_g325_battery_request),
	.reply_timeout = LOGI_G325_REPLY_TIMEOUT,
	.parse = logi_g325_parse,
};

static int logi_headset_raw_event(struct hid_device *hdev,
				  struct hid_report *report, u8 *data, int size)
{
	struct logi_headset *headset = hid_get_drvdata(hdev);
	struct logi_headset_reading reading = {};
	enum logi_headset_frame frame;
	unsigned long flags;
	bool changed = false;

	if (size < 1 || data[0] != headset->info->report_id)
		return 0;

	frame = headset->info->parse(data, size, &reading);
	if (frame == LOGI_HEADSET_FRAME_INVALID)
		hid_dbg_ratelimited(hdev, "ignoring a battery frame with invalid values\n");

	spin_lock_irqsave(&headset->lock, flags);
	switch (frame) {
	case LOGI_HEADSET_FRAME_ACK:
		headset->acked = true;
		break;
	case LOGI_HEADSET_FRAME_BATTERY:
		headset->capacity = reading.capacity;
		headset->charging = reading.charging;
		headset->present = true;
		headset->awaiting = false;
		changed = true;
		break;
	case LOGI_HEADSET_FRAME_POWER_OFF:
		/* Nothing to wait for from a headset that is off. */
		headset->awaiting = false;
		if (headset->present) {
			headset->present = false;
			changed = true;
		}
		break;
	case LOGI_HEADSET_FRAME_POWER_ON:
	case LOGI_HEADSET_FRAME_CHANGED:
		if (!headset->wake_queued && !headset->removing) {
			/*
			 * The headset came on or its battery changed. Only the first
			 * such event moves the request forward, so a burst of them
			 * cannot keep pushing it back.
			 */
			headset->wake_queued = true;
			mod_delayed_work(system_dfl_wq, &headset->poll,
					 LOGI_HEADSET_WAKE_DELAY);
		}
		break;
	case LOGI_HEADSET_FRAME_INVALID:
	case LOGI_HEADSET_FRAME_IGNORE:
		break;
	}
	spin_unlock_irqrestore(&headset->lock, flags);

	if (changed)
		power_supply_changed(headset->battery);

	/* Let the frame reach hidraw as well, userspace tools read it too. */
	return 0;
}

static int logi_headset_probe(struct hid_device *hdev,
			      const struct hid_device_id *id)
{
	struct power_supply_config cfg = {};
	const struct logi_headset_info *info;
	struct logi_headset *headset;
	const char *name;
	int ret;

	/* A device added through new_id has no driver data. */
	info = (const struct logi_headset_info *)id->driver_data;
	if (!info)
		return -EINVAL;

	headset = devm_kzalloc(&hdev->dev, sizeof(*headset), GFP_KERNEL);
	if (!headset)
		return -ENOMEM;

	headset->hdev = hdev;
	headset->info = info;
	spin_lock_init(&headset->lock);
	INIT_DELAYED_WORK(&headset->poll, logi_headset_poll);
	INIT_DELAYED_WORK(&headset->timeout, logi_headset_timeout);
	hid_set_drvdata(hdev, headset);

	/* As hid-generic did, so that the media keys keep their own input device. */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
			      "logitech-headset-%d-battery", hdev->id);
	if (!name)
		return -ENOMEM;

	headset->desc.name = name;
	headset->desc.type = POWER_SUPPLY_TYPE_BATTERY;
	headset->desc.properties = logi_headset_properties;
	headset->desc.num_properties = ARRAY_SIZE(logi_headset_properties);
	headset->desc.get_property = logi_headset_get_property;
	cfg.drv_data = headset;

	headset->battery = devm_power_supply_register(&hdev->dev, &headset->desc,
						      &cfg);
	if (IS_ERR(headset->battery))
		return dev_err_probe(&hdev->dev, PTR_ERR(headset->battery),
				     "cannot register the battery\n");

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	/* Input reports only arrive while the transport is open. */
	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	queue_delayed_work(system_dfl_wq, &headset->poll, HZ);

	return 0;
}

static void logi_headset_remove(struct hid_device *hdev)
{
	struct logi_headset *headset = hid_get_drvdata(hdev);
	unsigned long flags;

	/* An event arriving after this point does not queue the poll again. */
	spin_lock_irqsave(&headset->lock, flags);
	headset->removing = true;
	spin_unlock_irqrestore(&headset->lock, flags);

	cancel_delayed_work_sync(&headset->poll);
	cancel_delayed_work_sync(&headset->timeout);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id logi_headset_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH,
			 USB_DEVICE_ID_LOGITECH_PRO_X_2_LIGHTSPEED),
	  .driver_data = (kernel_ulong_t)&logi_pro_x_2_info },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH,
			 USB_DEVICE_ID_LOGITECH_G325_LIGHTSPEED),
	  .driver_data = (kernel_ulong_t)&logi_g325_info },
	{ }
};
MODULE_DEVICE_TABLE(hid, logi_headset_devices);

static struct hid_driver logi_headset_driver = {
	.name = "logitech-headset",
	.id_table = logi_headset_devices,
	.probe = logi_headset_probe,
	.remove = logi_headset_remove,
	.raw_event = logi_headset_raw_event,
};
module_hid_driver(logi_headset_driver);

MODULE_AUTHOR("Méven Car <meven@kde.org>");
MODULE_DESCRIPTION("Battery for Logitech wireless headsets without HID++");
MODULE_LICENSE("GPL");
