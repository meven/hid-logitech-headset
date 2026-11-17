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

/*
 * Both vendor collections exchange fixed size frames, and a request is a single
 * frame with no handshake. What differs between the headsets is the report
 * they use and the layout of the frames, so each one has a logi_headset_info.
 */
#define LOGI_HEADSET_FRAME_SIZE		64

#define LOGI_HEADSET_POLL_INTERVAL	(120 * HZ)

/* Wait between a power-on event and the request, so the radio link settles. */
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

/* What a frame turned out to be, once the headset specific parser read it. */
enum logi_headset_frame {
	LOGI_HEADSET_FRAME_IGNORE,	/* not about the battery or the power */
	LOGI_HEADSET_FRAME_BATTERY,	/* the reading holds a level and a state */
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
	enum logi_headset_frame (*parse)(const u8 *data, int size,
					 struct logi_headset_reading *reading);
};

struct logi_headset {
	struct hid_device *hdev;
	const struct logi_headset_info *info;
	struct power_supply *battery;
	struct power_supply_desc desc;
	struct delayed_work poll;
	spinlock_t lock;	/* guards the values below */
	int capacity;
	bool charging;
	bool present;		/* capacity and charging hold a reading from the headset */
	bool wake_queued;	/* a power-on event moved the next request forward */
	bool removing;		/* power events no longer queue a request */
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
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&headset->lock, flags);
	headset->wake_queued = false;
	spin_unlock_irqrestore(&headset->lock, flags);

	ret = logi_headset_ask(headset);
	if (ret < 0)
		hid_dbg(headset->hdev, "battery request failed: %d\n", ret);

	queue_delayed_work(system_dfl_wq, &headset->poll, LOGI_HEADSET_POLL_INTERVAL);
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

	spin_lock_irqsave(&headset->lock, flags);
	switch (frame) {
	case LOGI_HEADSET_FRAME_BATTERY:
		headset->capacity = reading.capacity;
		headset->charging = reading.charging;
		headset->present = true;
		changed = true;
		break;
	case LOGI_HEADSET_FRAME_POWER_OFF:
		if (headset->present) {
			headset->present = false;
			changed = true;
		}
		break;
	case LOGI_HEADSET_FRAME_POWER_ON:
		if (!headset->wake_queued && !headset->removing) {
			/*
			 * The headset came on. Only the first such event moves the
			 * request forward, so a burst of them cannot keep pushing it
			 * back.
			 */
			headset->wake_queued = true;
			mod_delayed_work(system_dfl_wq, &headset->poll,
					 LOGI_HEADSET_WAKE_DELAY);
		}
		break;
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

	/* A power event arriving after this point does not queue the poll again. */
	spin_lock_irqsave(&headset->lock, flags);
	headset->removing = true;
	spin_unlock_irqrestore(&headset->lock, flags);

	cancel_delayed_work_sync(&headset->poll);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id logi_headset_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH,
			 USB_DEVICE_ID_LOGITECH_PRO_X_2_LIGHTSPEED),
	  .driver_data = (kernel_ulong_t)&logi_pro_x_2_info },
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
