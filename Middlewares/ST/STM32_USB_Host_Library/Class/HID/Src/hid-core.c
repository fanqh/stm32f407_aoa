/*
 *  HID support for Linux
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2006-2012 Jiri Kosina
 *  Copyright (c) 2015 Tianfu Ma <matianfu@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */
#include <errno.h>

#include "hid.h"
#include "usbh_def.h"
#include "usbh_conf.h"

#define dbg_hid(...)    USBH_UsrLog(__VA_ARGS__)

static struct hid_device hid_device1;
static unsigned hid_device1_requested = 0;

/** from linux kernel tree linux/kernel.h **/
/*
 * ..and if you can't take the strict
 * types, you can specify one yourself.
 *
 * Or not use min/max/clamp at all, of course.
 */
#define min_t(type, x, y) ({                    \
        type __min1 = (x);                      \
        type __min2 = (y);                      \
        __min1 < __min2 ? __min1: __min2; })

#define max_t(type, x, y) ({                    \
        type __max1 = (x);                      \
        type __max2 = (y);                      \
        __max1 > __max2 ? __max1: __max2; })


static inline uint16_t get_unaligned_le16(const void *p)
{
    uint16_t tmp;
    memmove(&tmp, p, 2);
    return tmp;
}

static inline uint32_t get_unaligned_le32(const void *p)
{
    uint32_t tmp;
    memmove(&tmp, p, 4);
    return tmp;
}

static inline uint64_t get_unaligned_le64(const void *p)
{
    uint64_t tmp;
    memmove(&tmp, p, 8);
    return tmp;
}

/**
 * from include/linux/kernel.h
 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

int32_t hid_snto32(uint32_t value, unsigned n);

static const char *hid_gpd_strings[] =
{ "Undefined", "Pointer", "Mouse", "Reserved", "Joystick", "Gamepad", "Keyboard",
    "Keypad", "Multi-Axis Controller" };

/*
 * Register a new report for a device.
 */
struct hid_report *hid_register_report(struct hid_device *device, unsigned type,
    unsigned id)
{
  struct hid_report_enum *report_enum = device->report_enum + type;
  struct hid_report *report;

  // USBH_UsrLog("hid_register_report, type: %d, id: %d", type, id);

  if (id >= HID_MAX_IDS)
    return NULL;
  if (report_enum->report_id_hash[id])
    return report_enum->report_id_hash[id];

  if (report_enum->report_array_size == HID_MAX_REPORTS_PER_TYPE)
  {
    USBH_UsrLog("HID error: Max reports per type (%d) reached.", HID_MAX_REPORTS_PER_TYPE);
    return NULL;
  }

  report = &report_enum->report_array[report_enum->report_array_size];
  memset(report, 0, sizeof(struct hid_report));

  if (id != 0)
    report_enum->numbered = 1;

  report->id = id;
  report->type = type;
  report->size = 0;
  report->device = device;
  report_enum->report_id_hash[id] = report;

  report_enum->report_array_size++;

  return report;

}

/*
 * Request field, usage and values memory from pool
 * return 0 for success, others for error
 */
static int hid_request_field(struct hid_device* dev, unsigned usages, unsigned values,
    struct hid_field_request* req)
{
  if ((!dev) || (!req)) {
    return -1;  // ERROR
  }

  if (dev->field_pool_position >= HID_FIELD_POOL_SIZE ||
      dev->usage_pool_position + usages >= HID_USAGE_POOL_SIZE ||
      dev->value_pool_position + values >= HID_VALUE_POOL_SIZE)
  {
    return -1;  // ERROR
  }

  req->field = &dev->field_pool[dev->field_pool_position];
  req->usage = &dev->usage_pool[dev->usage_pool_position];
  req->value = &dev->value_pool[dev->value_pool_position];

  memset(req->field, 0, sizeof(struct hid_field));
  memset(req->usage, 0, usages * sizeof(struct hid_usage));
  memset(req->value, 0, values * sizeof(unsigned int));

  dev->field_pool_position += 1;
  dev->usage_pool_position += usages;
  dev->value_pool_position += values;

  return 0;
}

/*
 * Register a new field for this report.
 */
static struct hid_field *hid_register_field(struct hid_report *report, unsigned usages, unsigned values)
{
	struct hid_field *field;
	struct hid_field_request request;
	size_t size;

	static unsigned int total_field_size = 0;

	if (report->maxfield == HID_MAX_FIELDS) {
		hid_err(report->device, "too many fields in report\n");
		return NULL;
	}

	size = (sizeof(struct hid_field) +
        usages * sizeof(struct hid_usage) +
        values * sizeof(unsigned));

	if (0 != hid_request_field(report->device, usages, values, &request)) {
	  return NULL;
	}

	total_field_size += size;
	USBH_UsrLog("total field size: %d", total_field_size);
	USBH_UsrLog("  size of struct hid_filed: %d", sizeof(struct hid_field));
	USBH_UsrLog("  usages: %d, size of struct hid_usage %d", usages, sizeof(struct hid_usage));
	USBH_UsrLog("  values: %d, size of unsigned %d", values, sizeof(unsigned));

	field = request.field;
	field->index = report->maxfield++;
	report->field[field->index] = field;
//	field->usage = (struct hid_usage *)(field + 1);
//	field->value = (int32_t *)(field->usage + usages);
	field->usage = request.usage;
	field->value = request.value;
	field->report = report;

	return field;
}


/*
 * Open a collection. The type/usage is pushed on the stack.
 */
static int open_collection(struct hid_parser *parser, unsigned type)
{
  struct hid_collection *collection;
  unsigned usage;

// USBH_UsrLog("open collection");

  usage = parser->local.usage[0];

  if (parser->collection_stack_ptr == HID_COLLECTION_STACK_SIZE)
  {
    hid_err(parser->device, "collection stack overflow\n");
    return -EINVAL;
  }

  if (parser->device->maxcollection == parser->device->collection_size)
  {
    return -ENOMEM;
  }

#if 0
  /** this code try to re-allocate collect pointer array, double **/
  if (parser->device->maxcollection == parser->device->collection_size)
  {
    // collection = kmalloc(sizeof(struct hid_collection) *
    //		parser->device->collection_size * 2, GFP_KERNEL);
    collection = malloc(
        sizeof(struct hid_collection) * parser->device->collection_size * 2);

    printf("\r\nmalloc for collection...\r\n\r\n");

    if (collection == NULL)
    {
      hid_err(parser->device, "failed to reallocate collection array\n");
      return -ENOMEM;
    }
    memcpy(collection, parser->device->collection,
        sizeof(struct hid_collection) * parser->device->collection_size);
    memset(collection + parser->device->collection_size, 0,
        sizeof(struct hid_collection) * parser->device->collection_size);
    free(parser->device->collection);
    parser->device->collection = collection;
    parser->device->collection_size *= 2;
  }
#endif

  parser->collection_stack[parser->collection_stack_ptr++] =
      parser->device->maxcollection;

  collection = parser->device->collection + parser->device->maxcollection++;
  collection->type = type;
  collection->usage = usage;
  collection->level = parser->collection_stack_ptr - 1;

  if (type == HID_COLLECTION_APPLICATION)
    parser->device->maxapplication++;

  return 0;
}

/*
 * Close a collection.
 */
static int close_collection(struct hid_parser *parser)
{
  // USBH_UsrLog("close collection");

  if (!parser->collection_stack_ptr)
  {
    hid_err(parser->device, "collection stack underflow\n");
    return -EINVAL;
  }
  parser->collection_stack_ptr--;
  return 0;
}

/*
 * Climb up the stack, search for the specified collection type
 * and return the usage.
 */
static unsigned hid_lookup_collection(struct hid_parser *parser, unsigned type)
{
	struct hid_collection *collection = parser->device->collection;
	int n;

	for (n = parser->collection_stack_ptr - 1; n >= 0; n--) {
		unsigned index = parser->collection_stack[n];
		if (collection[index].type == type)
			return collection[index].usage;
	}
	return 0; /* we know nothing about this usage type */
}

/*
 * Add a usage to the temporary parser table.
 */
static int hid_add_usage(struct hid_parser *parser, unsigned usage)
{
  if (parser->local.usage_index >= HID_MAX_USAGES)
  {
    hid_err(parser->device, "usage index exceeded\n");
    return -1;
  }
  parser->local.usage[parser->local.usage_index] = usage;
  parser->local.collection_index[parser->local.usage_index] =
      parser->collection_stack_ptr ?
          parser->collection_stack[parser->collection_stack_ptr - 1] : 0;
  parser->local.usage_index++;

//  USBH_UsrLog("  parser->local.usage_index++ %d", parser->local.usage_index);

  return 0;
}

/*
 * Register a new field for this report.
 */
static int hid_add_field(struct hid_parser *parser, unsigned report_type, unsigned flags)
{
	struct hid_report *report;
	struct hid_field *field;
	unsigned usages;
	unsigned offset;
	unsigned i;

	// USBH_UsrLog("hid_add_field");

	report = hid_register_report(parser->device, report_type, parser->global.report_id);
	if (!report) {
		hid_err(parser->device, "hid_register_report failed\n");
		return -1;
	}

	/* Handle both signed and unsigned cases properly */
	if ((parser->global.logical_minimum < 0 &&
		parser->global.logical_maximum <
		parser->global.logical_minimum) ||
		(parser->global.logical_minimum >= 0 &&
		(uint32_t)parser->global.logical_maximum <         /** modified **/
		(uint32_t)parser->global.logical_minimum)) {       /** modified **/

		dbg_hid("logical range invalid 0x%x 0x%x\n",
			(unsigned int)parser->global.logical_minimum,
			(unsigned int)parser->global.logical_maximum);
		return -1;
	}

	offset = report->size;
	report->size += parser->global.report_size * parser->global.report_count;

	if (!parser->local.usage_index) /* Ignore padding fields */
		return 0;

	usages = max_t(unsigned, parser->local.usage_index,
				 parser->global.report_count);

	field = hid_register_field(report, usages, parser->global.report_count);
	if (!field)
		return 0;

	field->physical = hid_lookup_collection(parser, HID_COLLECTION_PHYSICAL);
	field->logical = hid_lookup_collection(parser, HID_COLLECTION_LOGICAL);
	field->application = hid_lookup_collection(parser, HID_COLLECTION_APPLICATION);

	for (i = 0; i < usages; i++) {
		unsigned j = i;
		/* Duplicate the last usage we parsed if we have excess values */
		if (i >= parser->local.usage_index)
			j = parser->local.usage_index - 1;
		field->usage[i].hid = parser->local.usage[j];
		field->usage[i].collection_index =
			parser->local.collection_index[j];
		field->usage[i].usage_index = i;
	}

	field->maxusage = usages;
	field->flags = flags;
	field->report_offset = offset;
	field->report_type = report_type;
	field->report_size = parser->global.report_size;
	field->report_count = parser->global.report_count;
	field->logical_minimum = parser->global.logical_minimum;
	field->logical_maximum = parser->global.logical_maximum;
	field->physical_minimum = parser->global.physical_minimum;
	field->physical_maximum = parser->global.physical_maximum;
	field->unit_exponent = parser->global.unit_exponent;
	field->unit = parser->global.unit;

	return 0;
}

/*
 * Read data value from item.
 */


//static u32 item_udata(struct hid_item *item)
//{
//	switch (item->size) {
//	case 1: return item->data.u8;
//	case 2: return item->data.u16;
//	case 4: return item->data.u32;
//	}
//	return 0;
//}

static uint32_t item_udata(struct hid_item *item)
{
    switch (item->size) {
    case 1: return item->data.u8;
    case 2: return item->data.u16;
    case 4: return item->data.u32;
    }
    return 0;
}


//static s32 item_sdata(struct hid_item *item)
//{
//	switch (item->size) {
//	case 1: return item->data.s8;
//	case 2: return item->data.s16;
//	case 4: return item->data.s32;
//	}
//	return 0;
//}

static int32_t item_sdata(struct hid_item *item)
{
    switch (item->size) {
    case 1: return item->data.s8;
    case 2: return item->data.s16;
    case 4: return item->data.s32;
    }
    return 0;
}


/*
 * Process a global item.
 */
static int hid_parser_global(struct hid_parser *parser, struct hid_item *item)
{
	// __s32 raw_value;
  int raw_value;

    // USBH_UsrLog("hid_parser_global");

	switch (item->tag) {
	case HID_GLOBAL_ITEM_TAG_PUSH:

		if (parser->global_stack_ptr == HID_GLOBAL_STACK_SIZE) {
			hid_err(parser->device, "global environment stack overflow\n");
			return -1;
		}

		memcpy(parser->global_stack + parser->global_stack_ptr++,
			&parser->global, sizeof(struct hid_global));
		return 0;

	case HID_GLOBAL_ITEM_TAG_POP:

		if (!parser->global_stack_ptr) {
			hid_err(parser->device, "global environment stack underflow\n");
			return -1;
		}

		memcpy(&parser->global, parser->global_stack +
			--parser->global_stack_ptr, sizeof(struct hid_global));
		return 0;

	case HID_GLOBAL_ITEM_TAG_USAGE_PAGE:
		parser->global.usage_page = item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_LOGICAL_MINIMUM:
		parser->global.logical_minimum = item_sdata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_LOGICAL_MAXIMUM:
		if (parser->global.logical_minimum < 0)
			parser->global.logical_maximum = item_sdata(item);
		else
			parser->global.logical_maximum = item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_PHYSICAL_MINIMUM:
		parser->global.physical_minimum = item_sdata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_PHYSICAL_MAXIMUM:
		if (parser->global.physical_minimum < 0)
			parser->global.physical_maximum = item_sdata(item);
		else
			parser->global.physical_maximum = item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_UNIT_EXPONENT:
		/* Many devices provide unit exponent as a two's complement
		 * nibble due to the common misunderstanding of HID
		 * specification 1.11, 6.2.2.7 Global Items. Attempt to handle
		 * both this and the standard encoding. */
		raw_value = item_sdata(item);
		if (!(raw_value & 0xfffffff0))
			parser->global.unit_exponent = hid_snto32(raw_value, 4);
		else
			parser->global.unit_exponent = raw_value;
		return 0;

	case HID_GLOBAL_ITEM_TAG_UNIT:
		parser->global.unit = item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_REPORT_SIZE:
		parser->global.report_size = item_udata(item);
		if (parser->global.report_size > 128) {
			hid_err(parser->device, "invalid report_size %d\n",
					parser->global.report_size);
			return -1;
		}
		return 0;

	case HID_GLOBAL_ITEM_TAG_REPORT_COUNT:
		parser->global.report_count = item_udata(item);
		if (parser->global.report_count > HID_MAX_USAGES) {
			hid_err(parser->device, "invalid report_count %d\n",
					parser->global.report_count);
			return -1;
		}
		return 0;

	case HID_GLOBAL_ITEM_TAG_REPORT_ID:
		parser->global.report_id = item_udata(item);
		if (parser->global.report_id == 0 ||
		    parser->global.report_id >= HID_MAX_IDS) {
			hid_err(parser->device, "report_id %u is invalid\n",
				parser->global.report_id);
			return -1;
		}
		return 0;

	default:
		hid_err(parser->device, "unknown global tag 0x%x\n", item->tag);
		return -1;
	}
}

/*
 * Process a local item.
 */
static int hid_parser_local(struct hid_parser *parser, struct hid_item *item)
{
	// __u32 data;
    uint32_t data;
	unsigned n;

	// USBH_UsrLog("hid_parser_local");

	data = item_udata(item);

	switch (item->tag) {
	case HID_LOCAL_ITEM_TAG_DELIMITER:

		if (data) {
			/*
			 * We treat items before the first delimiter
			 * as global to all usage sets (branch 0).
			 * In the moment we process only these global
			 * items and the first delimiter set.
			 */
			if (parser->local.delimiter_depth != 0) {
				hid_err(parser->device, "nested delimiters\n");
				return -1;
			}
			parser->local.delimiter_depth++;
			parser->local.delimiter_branch++;
		} else {
			if (parser->local.delimiter_depth < 1) {
				hid_err(parser->device, "bogus close delimiter\n");
				return -1;
			}
			parser->local.delimiter_depth--;
		}
		return 0;

	case HID_LOCAL_ITEM_TAG_USAGE:

		if (parser->local.delimiter_branch > 1) {
			dbg_hid("alternative usage ignored\n");
			return 0;
		}

		if (item->size <= 2)
			data = (parser->global.usage_page << 16) + data;

		return hid_add_usage(parser, data);

	case HID_LOCAL_ITEM_TAG_USAGE_MINIMUM:

		if (parser->local.delimiter_branch > 1) {
			dbg_hid("alternative usage ignored\n");
			return 0;
		}

		if (item->size <= 2)
			data = (parser->global.usage_page << 16) + data;

		parser->local.usage_minimum = data;
		return 0;

	case HID_LOCAL_ITEM_TAG_USAGE_MAXIMUM:

		if (parser->local.delimiter_branch > 1) {
			dbg_hid("alternative usage ignored\n");
			return 0;
		}

		if (item->size <= 2)
			data = (parser->global.usage_page << 16) + data;

		for (n = parser->local.usage_minimum; n <= data; n++)
			if (hid_add_usage(parser, n)) {
				dbg_hid("hid_add_usage failed\n");
				return -1;
			}
		return 0;

	default:

		dbg_hid("unknown local item tag 0x%x\n", item->tag);
		return 0;
	}
	return 0;
}

/*
 * Process a main item.
 */
static int hid_parser_main(struct hid_parser *parser, struct hid_item *item)
{
    uint32_t data;
	int ret;

	// USBH_UsrLog("hid_parser_main");

	data = item_udata(item);

	switch (item->tag) {
	case HID_MAIN_ITEM_TAG_BEGIN_COLLECTION:
		ret = open_collection(parser, data & 0xff);
		break;
	case HID_MAIN_ITEM_TAG_END_COLLECTION:
		ret = close_collection(parser);
		break;
	case HID_MAIN_ITEM_TAG_INPUT:
		ret = hid_add_field(parser, HID_INPUT_REPORT, data);
		break;
	case HID_MAIN_ITEM_TAG_OUTPUT:
		ret = hid_add_field(parser, HID_OUTPUT_REPORT, data);
		break;
	case HID_MAIN_ITEM_TAG_FEATURE:
		ret = hid_add_field(parser, HID_FEATURE_REPORT, data);
		break;
	default:
		hid_err(parser->device, "unknown main item tag 0x%x\n", item->tag);
		ret = 0;
	}

	memset(&parser->local, 0, sizeof(parser->local));	/* Reset the local parser environment */

	return ret;
}

/*
 * Process a reserved item.
 */
static int hid_parser_reserved(struct hid_parser *parser, struct hid_item *item)
{
	dbg_hid("reserved item type, tag 0x%x\n", item->tag);
	return 0;
}


/*
 * Close report. This function returns the device
 * state to the point prior to hid_open_report().
 */
void hid_close_report(struct hid_device *device)
{
  unsigned i;

  for (i = 0; i < HID_REPORT_TYPES; i++)
  {
    struct hid_report_enum *report_enum = device->report_enum + i;
    memset(report_enum, 0, sizeof(*report_enum));
  }

  device->dev_rsize = 0;
  device->collection_size = 0;
  device->maxcollection = 0;
  device->maxapplication = 0;

  device->status &= ~HID_STAT_PARSED;
}

/*
 * Fetch a report description item from the data stream. We support long
 * items, though they are not used yet.
 */
// static u8 *fetch_item(__u8 *start, __u8 *end, struct hid_item *item)
static uint8_t *fetch_item(uint8_t *start, uint8_t *end, struct hid_item *item)
{
  // u8 b;
  uint8_t b;

  if ((end - start) <= 0)
    return NULL ;

  b = *start++;

  item->type = (b >> 2) & 3;
  item->tag = (b >> 4) & 15;

  if (item->tag == HID_ITEM_TAG_LONG)
  {

    item->format = HID_ITEM_FORMAT_LONG;

    if ((end - start) < 2)
      return NULL ;

    item->size = *start++;
    item->tag = *start++;

    if ((end - start) < item->size)
      return NULL ;

    item->data.longdata = start;
    start += item->size;
    return start;
  }

  item->format = HID_ITEM_FORMAT_SHORT;
  item->size = b & 3;

//	USBH_UsrLog("fetch item %02x, type: %d, tag: %d, size: %d",
//	    b, item->type, item->tag, item->size);

  switch (item->size)
  {
  case 0:
    // USBH_UsrLog("  no value");
    return start;
    break;

  case 1:
    if ((end - start) < 1)
      return NULL ;
    item->data.u8 = *start++;
    // USBH_UsrLog("  %02x", item->data.u8);
    return start;

  case 2:
    if ((end - start) < 2)
      return NULL ;
    item->data.u16 = get_unaligned_le16(start);
    // start = (__u8 *)((__le16 *)start + 1);
    start = (uint8_t*) ((uint16_t*) start + 1);
    // USBH_UsrLog("  %04x", item->data.u16);
    return start;

  case 3:
    item->size++;
    if ((end - start) < 4)
      return NULL ;
    item->data.u32 = get_unaligned_le32(start);
    // start = (__u8 *)((__le32 *)start + 1);
    start = (uint8_t*) ((uint32_t*) start + 1);
    // USBH_UsrLog("  %08x", (unsigned int)item->data.u32);
    return start;
  }

  return NULL ;
}

#if 0

static void hid_scan_input_usage(struct hid_parser *parser, u32 usage)
{
	struct hid_device *hid = parser->device;

	if (usage == HID_DG_CONTACTID)
		hid->group = HID_GROUP_MULTITOUCH;
}

static void hid_scan_feature_usage(struct hid_parser *parser, u32 usage)
{
	if (usage == 0xff0000c5 && parser->global.report_count == 256 &&
	    parser->global.report_size == 8)
		parser->scan_flags |= HID_SCAN_FLAG_MT_WIN_8;
}

static void hid_scan_collection(struct hid_parser *parser, unsigned type)
{
	struct hid_device *hid = parser->device;

	if (((parser->global.usage_page << 16) == HID_UP_SENSOR) &&
	    type == HID_COLLECTION_PHYSICAL)
		hid->group = HID_GROUP_SENSOR_HUB;
}

static int hid_scan_main(struct hid_parser *parser, struct hid_item *item)
{
	__u32 data;
	int i;

	data = item_udata(item);

	switch (item->tag) {
	case HID_MAIN_ITEM_TAG_BEGIN_COLLECTION:
		hid_scan_collection(parser, data & 0xff);
		break;
	case HID_MAIN_ITEM_TAG_END_COLLECTION:
		break;
	case HID_MAIN_ITEM_TAG_INPUT:
		/* ignore constant inputs, they will be ignored by hid-input */
		if (data & HID_MAIN_ITEM_CONSTANT)
			break;
		for (i = 0; i < parser->local.usage_index; i++)
			hid_scan_input_usage(parser, parser->local.usage[i]);
		break;
	case HID_MAIN_ITEM_TAG_OUTPUT:
		break;
	case HID_MAIN_ITEM_TAG_FEATURE:
		for (i = 0; i < parser->local.usage_index; i++)
			hid_scan_feature_usage(parser, parser->local.usage[i]);
		break;
	}

	/* Reset the local parser environment */
	memset(&parser->local, 0, sizeof(parser->local));

	return 0;
}

/*
 * Scan a report descriptor before the device is added to the bus.
 * Sets device groups and other properties that determine what driver
 * to load.
 */
static int hid_scan_report(struct hid_device *hid)
{
	struct hid_parser *parser;
	struct hid_item item;
	__u8 *start = hid->dev_rdesc;
	__u8 *end = start + hid->dev_rsize;
	static int (*dispatch_type[])(struct hid_parser *parser,
				      struct hid_item *item) = {
		hid_scan_main,
		hid_parser_global,
		hid_parser_local,
		hid_parser_reserved
	};

	parser = vzalloc(sizeof(struct hid_parser));
	if (!parser)
		return -ENOMEM;

	parser->device = hid;
	hid->group = HID_GROUP_GENERIC;

	/*
	 * The parsing is simpler than the one in hid_open_report() as we should
	 * be robust against hid errors. Those errors will be raised by
	 * hid_open_report() anyway.
	 */
	while ((start = fetch_item(start, end, &item)) != NULL)
		dispatch_type[item.type](parser, &item);

	/*
	 * Handle special flags set during scanning.
	 */
	if ((parser->scan_flags & HID_SCAN_FLAG_MT_WIN_8) &&
	    (hid->group == HID_GROUP_MULTITOUCH))
		hid->group = HID_GROUP_MULTITOUCH_WIN_8;

	vfree(parser);
	return 0;
}

/**
 * hid_parse_report - parse device report
 *
 * @device: hid device
 * @start: report start
 * @size: report size
 *
 * Allocate the device report as read by the bus driver. This function should
 * only be called from parse() in ll drivers.
 */
int hid_parse_report(struct hid_device *hid, __u8 *start, unsigned size)
{
	hid->dev_rdesc = kmemdup(start, size, GFP_KERNEL);
	if (!hid->dev_rdesc)
		return -ENOMEM;
	hid->dev_rsize = size;
	return 0;
}

static const char * const hid_report_names[] = {
	"HID_INPUT_REPORT",
	"HID_OUTPUT_REPORT",
	"HID_FEATURE_REPORT",
};
/**
 * hid_validate_values - validate existing device report's value indexes
 *
 * @device: hid device
 * @type: which report type to examine
 * @id: which report ID to examine (0 for first)
 * @field_index: which report field to examine
 * @report_counts: expected number of values
 *
 * Validate the number of values in a given field of a given report, after
 * parsing.
 */
struct hid_report *hid_validate_values(struct hid_device *hid,
				       unsigned int type, unsigned int id,
				       unsigned int field_index,
				       unsigned int report_counts)
{
	struct hid_report *report;

	if (type > HID_FEATURE_REPORT) {
		hid_err(hid, "invalid HID report type %u\n", type);
		return NULL;
	}

	if (id >= HID_MAX_IDS) {
		hid_err(hid, "invalid HID report id %u\n", id);
		return NULL;
	}

	/*
	 * Explicitly not using hid_get_report() here since it depends on
	 * ->numbered being checked, which may not always be the case when
	 * drivers go to access report values.
	 */
	if (id == 0) {
		/*
		 * Validating on id 0 means we should examine the first
		 * report in the list.
		 */
		report = list_entry(
				hid->report_enum[type].report_list.next,
				struct hid_report, list);
	} else {
		report = hid->report_enum[type].report_id_hash[id];
	}
	if (!report) {
		hid_err(hid, "missing %s %u\n", hid_report_names[type], id);
		return NULL;
	}
	if (report->maxfield <= field_index) {
		hid_err(hid, "not enough fields in %s %u\n",
			hid_report_names[type], id);
		return NULL;
	}
	if (report->field[field_index]->report_count < report_counts) {
		hid_err(hid, "not enough values in %s %u field %u\n",
			hid_report_names[type], id, field_index);
		return NULL;
	}
	return report;
}

#endif

/**
 * hid_set_report_descriptor
 *
 *
 */
int hid_set_report_descriptor(struct hid_device *hiddev, uint8_t* rdesc,
    uint16_t rsize)
{
  if (hiddev == NULL || rdesc == NULL)
  {
    return -1;
  }

  if (rsize == 0 || rsize > HID_REPORT_DESCRIPTOR_SIZE)
  {
    USBH_UsrLog("report descriptor size error.");
    return -1;
  }
  memcpy(hiddev->dev_rdesc, rdesc, rsize);
  hiddev->dev_rsize = rsize;
  return 0;
}

/**
 * hid_open_report - open a driver-specific device report
 *
 * @device: hid device
 *
 * Parse a report description into a hid_device structure. Reports are
 * enumerated, fields are attached to these reports.
 * 0 returned on success, otherwise nonzero error value.
 *
 * This function (or the equivalent hid_parse() macro) should only be
 * called from probe() in drivers, before starting the device.
 *
 * This function allocate a parser then destroy it.
 *
 */
int hid_open_report(struct hid_device *device)
{
  static struct hid_parser hid_parser;

  struct hid_parser *parser = &hid_parser;
  struct hid_item item;
  unsigned int size;
  uint8_t *start;
  uint8_t *end;
  int ret;

  static int (*dispatch_type[])(struct hid_parser *parser,
      struct hid_item *item) =
      {
        hid_parser_main,
        hid_parser_global,
        hid_parser_local,
        hid_parser_reserved
      };

  start = device->dev_rdesc;
  size = device->dev_rsize;

  memset(parser, 0, sizeof(struct hid_parser));
  parser->device = device;

  end = start + size;

  device->collection_size = HID_DEFAULT_NUM_COLLECTIONS;

  ret = -EINVAL;
  while ((start = fetch_item(start, end, &item)) != NULL )
  {

    if (item.format != HID_ITEM_FORMAT_SHORT)
    {
      hid_err(device, "unexpected long global item\n");
      goto err;
    }

    if (dispatch_type[item.type](parser, &item))
    {
      hid_err(device, "item %u %u %u %u parsing failed\n",
          item.format, (unsigned)item.size,
          (unsigned)item.type, (unsigned)item.tag);
      goto err;
    }

    if (start == end)
    {
      if (parser->collection_stack_ptr)
      {
        hid_err(device, "unbalanced collection at end of report description\n");
        goto err;
      }
      if (parser->local.delimiter_depth)
      {
        hid_err(device, "unbalanced delimiter at end of report description\n");
        goto err;
      }

      device->status |= HID_STAT_PARSED;
      return 0;
    }
  }

  hid_err(device, "item fetching failed at offset %d\n", (int)(end - start));

  err:

  hid_close_report(device);
  return ret;
}




/*
 * Convert a signed n-bit integer to signed 32-bit integer. Common
 * cases are done through the compiler, the screwed things has to be
 * done by hand.
 */

//static s32 snto32(__u32 value, unsigned n)
//{
//	switch (n) {
//	case 8:  return ((__s8)value);
//	case 16: return ((__s16)value);
//	case 32: return ((__s32)value);
//	}
//	return value & (1 << (n - 1)) ? value | (-1 << n) : value;
//}
static int32_t snto32(uint32_t value, unsigned n)
{
    switch (n) {
    case 8:  return ((int8_t)value);
    case 16: return ((int16_t)value);
    case 32: return ((int32_t)value);
    }
    return value & (1 << (n - 1)) ? value | (-1 << n) : value;
}

//s32 hid_snto32(__u32 value, unsigned n)
//{
//	return snto32(value, n);
//}

int32_t hid_snto32(uint32_t value, unsigned n)
{
    return snto32(value, n);
}
// EXPORT_SYMBOL_GPL(hid_snto32);

/*
 * Convert a signed 32-bit integer to a signed n-bit integer.
 */
//static u32 s32ton(__s32 value, unsigned n)
//{
//	s32 a = value >> (n - 1);
//	if (a && a != -1)
//		return value < 0 ? 1 << (n - 1) : (1 << (n - 1)) - 1;
//	return value & ((1 << n) - 1);
//}

//static uint32_t s32ton(int32_t value, unsigned n)
//{
//    int32_t a = value >> (n - 1);
//    if (a && a != -1)
//        return value < 0 ? 1 << (n - 1) : (1 << (n - 1)) - 1;
//    return value & ((1 << n) - 1);
//}

/*
 * Extract/implement a data field from/to a little endian report (bit array).
 *
 * Code sort-of follows HID spec:
 *     http://www.usb.org/developers/devclass_docs/HID1_11.pdf
 *
 * While the USB HID spec allows unlimited length bit fields in "report
 * descriptors", most devices never use more than 16 bits.
 * One model of UPS is claimed to report "LINEV" as a 32-bit field.
 * Search linux-kernel and linux-usb-devel archives for "hid-core extract".
 */

//static __u32 extract(const struct hid_device *hid, __u8 *report,
//		     unsigned offset, unsigned n)
//{
//	u64 x;
//
//	if (n > 32)
//		hid_warn(hid, "extract() called with n (%d) > 32! (%s)\n",
//			 n, current->comm);
//
//	report += offset >> 3;  /* adjust byte index */
//	offset &= 7;            /* now only need bit offset into one byte */
//	x = get_unaligned_le64(report);
//	x = (x >> offset) & ((1ULL << n) - 1);  /* extract bit field */
//	return (u32) x;
//}

static uint32_t extract(const struct hid_device *hid, uint8_t *report,
             unsigned offset, unsigned n)
{
    uint64_t x;

// TODO redefine hid_warn
//    if (n > 32)
//        hid_warn(hid, "extract() called with n (%d) > 32! (%s)\n",
//             n, current->comm);

    report += offset >> 3;  /* adjust byte index */
    offset &= 7;            /* now only need bit offset into one byte */
    x = get_unaligned_le64(report);
    x = (x >> offset) & ((1ULL << n) - 1);  /* extract bit field */
    return (uint32_t) x;
}

#if 0

/*
 * "implement" : set bits in a little endian bit stream.
 * Same concepts as "extract" (see comments above).
 * The data mangled in the bit stream remains in little endian
 * order the whole time. It make more sense to talk about
 * endianness of register values by considering a register
 * a "cached" copy of the little endiad bit stream.
 */
static void implement(const struct hid_device *hid, __u8 *report,
		      unsigned offset, unsigned n, __u32 value)
{
	u64 x;
	u64 m = (1ULL << n) - 1;

	if (n > 32)
		hid_warn(hid, "%s() called with n (%d) > 32! (%s)\n",
			 __func__, n, current->comm);

	if (value > m)
		hid_warn(hid, "%s() called with too large value %d! (%s)\n",
			 __func__, value, current->comm);
	WARN_ON(value > m);
	value &= m;

	report += offset >> 3;
	offset &= 7;

	x = get_unaligned_le64(report);
	x &= ~(m << offset);
	x |= ((u64)value) << offset;
	put_unaligned_le64(x, report);
}

#endif

/*
 * Search an array for a value.
 */
//static int search(__s32 *array, __s32 value, unsigned n)
//{
//  while (n--)
//  {
//    if (*array++ == value)
//      return 0;
//  }
//  return -1;
//}
static int search(int32_t *array, int32_t value, unsigned n)
{
  while (n--)
  {
    if (*array++ == value)
      return 0;
  }
  return -1;
}

#if 0

/**
 * hid_match_report - check if driver's raw_event should be called
 *
 * @hid: hid device
 * @report_type: type to match against
 *
 * compare hid->driver->report_table->report_type to report->type
 */
static int hid_match_report(struct hid_device *hid, struct hid_report *report)
{
	const struct hid_report_id *id = hid->driver->report_table;

	if (!id) /* NULL means all */
		return 1;

	for (; id->report_type != HID_TERMINATOR; id++)
		if (id->report_type == HID_ANY_ID ||
				id->report_type == report->type)
			return 1;
	return 0;
}

/**
 * hid_match_usage - check if driver's event should be called
 *
 * @hid: hid device
 * @usage: usage to match against
 *
 * compare hid->driver->usage_table->usage_{type,code} to
 * usage->usage_{type,code}
 */
static int hid_match_usage(struct hid_device *hid, struct hid_usage *usage)
{
	const struct hid_usage_id *id = hid->driver->usage_table;

	if (!id) /* NULL means all */
		return 1;

	for (; id->usage_type != HID_ANY_ID - 1; id++)
		if ((id->usage_hid == HID_ANY_ID ||
				id->usage_hid == usage->hid) &&
				(id->usage_type == HID_ANY_ID ||
				id->usage_type == usage->type) &&
				(id->usage_code == HID_ANY_ID ||
				 id->usage_code == usage->code))
			return 1;
	return 0;
}

#endif

//static void hid_process_event(struct hid_device *hid, struct hid_field *field,
//		struct hid_usage *usage, __s32 value, int interrupt)
//{
//	struct hid_driver *hdrv = hid->driver;
//	int ret;
//
//	if (!list_empty(&hid->debug_list))
//		hid_dump_input(hid, usage, value);
//
//	if (hdrv && hdrv->event && hid_match_usage(hid, usage)) {
//		ret = hdrv->event(hid, field, usage, value);
//		if (ret != 0) {
//			if (ret < 0)
//				hid_err(hid, "%s's event failed with %d\n",
//						hdrv->name, ret);
//			return;
//		}
//	}
//
//	if (hid->claimed & HID_CLAIMED_INPUT)
//		hidinput_hid_event(hid, field, usage, value);
//	if (hid->claimed & HID_CLAIMED_HIDDEV && interrupt && hid->hiddev_hid_event)
//		hid->hiddev_hid_event(hid, field, usage, value);
//}
extern void hidinput_hid_event(struct hid_device *hid, struct hid_field *field, struct hid_usage *usage, int32_t value);
//{
//  USBH_UsrLog("hidinput_hid_event, value: %d", value);
//}

//static void hid_process_event(struct hid_device *hid, struct hid_field *field, struct hid_usage *usage, int32_t value, int interrupt)
//{
////  struct hid_driver *hdrv = hid->driver;
////  int ret;
//
////    if (!list_empty(&hid->debug_list))
////        hid_dump_input(hid, usage, value);
//
////    if (hdrv && hdrv->event && hid_match_usage(hid, usage)) {
////        ret = hdrv->event(hid, field, usage, value);
////        if (ret != 0) {
////            if (ret < 0)
////                hid_err(hid, "%s's event failed with %d\n",
////                        hdrv->name, ret);
////            return;
////        }
////    }
//
////    if (hid->claimed & HID_CLAIMED_INPUT)
//        hidinput_hid_event(hid, field, usage, value);
////    if (hid->claimed & HID_CLAIMED_HIDDEV && interrupt && hid->hiddev_hid_event)
////        hid->hiddev_hid_event(hid, field, usage, value);
//}

#if 0 // see below

static void hid_input_field(struct hid_device *hid, struct hid_field *field,
                __u8 *data, int interrupt)
{
    unsigned n;
    unsigned count = field->report_count;
    unsigned offset = field->report_offset;
    unsigned size = field->report_size;
    __s32 min = field->logical_minimum;
    __s32 max = field->logical_maximum;
    __s32 *value;

    value = kmalloc(sizeof(__s32) * count, GFP_ATOMIC);
    if (!value)
        return;

    for (n = 0; n < count; n++) {

        value[n] = min < 0 ?
            snto32(extract(hid, data, offset + n * size, size),
                   size) :
            extract(hid, data, offset + n * size, size);

        /* Ignore report if ErrorRollOver */
        if (!(field->flags & HID_MAIN_ITEM_VARIABLE) &&
            value[n] >= min && value[n] <= max &&
            field->usage[value[n] - min].hid == HID_UP_KEYBOARD + 1)
            goto exit;
    }

    for (n = 0; n < count; n++) {

        if (HID_MAIN_ITEM_VARIABLE & field->flags) {
            hid_process_event(hid, field, &field->usage[n], value[n], interrupt);
            continue;
        }

        if (field->value[n] >= min && field->value[n] <= max
            && field->usage[field->value[n] - min].hid
            && search(value, field->value[n], count))
                hid_process_event(hid, field, &field->usage[field->value[n] - min], 0, interrupt);

        if (value[n] >= min && value[n] <= max
            && field->usage[value[n] - min].hid
            && search(field->value, value[n], count))
                hid_process_event(hid, field, &field->usage[value[n] - min], 1, interrupt);
    }

    memcpy(field->value, value, count * sizeof(__s32));
exit:
    kfree(value);
}

#endif

/*
 * Analyse a received field, and fetch the data from it. The field
 * content is stored for next report processing (we do differential
 * reporting to the layer).
 */
static void hid_input_field(struct hid_device *hid, struct hid_field *field, uint8_t *data)
{
  unsigned n;
  unsigned count = field->report_count;
  unsigned offset = field->report_offset;
  unsigned size = field->report_size;
  int32_t min = field->logical_minimum;
  int32_t max = field->logical_maximum;
  int32_t *value;

//  USBH_UsrLog(
//      "    hid_input_field, count: %d, size: %d, offset: %d, lmin: %d, lmax: %d",
//      count, size, offset, (int )min, (int )max);
//	value = kmalloc(sizeof(__s32) * count, GFP_ATOMIC);
//	if (!value)
//		return;
  value = malloc(sizeof(int32_t) * count);
  if (!value)
    return;

  // In this step, value is extracted and stored in temporary data structure.
  for (n = 0; n < count; n++)
  {

    value[n] =
        min < 0 ?
            snto32(extract(hid, data, offset + n * size, size), size) :
            extract(hid, data, offset + n * size, size);

    /* Ignore report if ErrorRollOver */
    if (!(field->flags & HID_MAIN_ITEM_VARIABLE) && value[n] >= min
        && value[n] <= max
        && field->usage[value[n] - min].hid == HID_UP_KEYBOARD + 1)
      goto exit;
  }

  for (n = 0; n < count; n++)
  {
    if (HID_MAIN_ITEM_VARIABLE & field->flags)
    {
      // hid_process_event(hid, field, &field->usage[n], value[n], interrupt);
      hidinput_hid_event(hid, field, &field->usage[n], value[n]);
      continue;
    }

    /* field->value is previously stored states */
    if (field->value[n] >= min && field->value[n] <= max
        && field->usage[field->value[n] - min].hid
        && search(value, field->value[n], count))
    {

      // USBH_UsrLog("    old field->value[%d] (%d) not found in new value", n, (int)field->value[n]);
      // hid_process_event(hid, field, &field->usage[field->value[n] - min], 0, interrupt);
      hidinput_hid_event(hid, field, &field->usage[field->value[n] - min], 0);
    }

    if (value[n] >= min && value[n] <= max && field->usage[value[n] - min].hid
        && search(field->value, value[n], count))
    {

      // USBH_UsrLog("    new value[%d] (%d) not found in old field->value", n, (int)value[n]);
      // hid_process_event(hid, field, &field->usage[value[n] - min], 1, interrupt);
      hidinput_hid_event(hid, field, &field->usage[value[n] - min], 1);
    }
  }

  memcpy(field->value, value, count * sizeof(int32_t));

exit:
  free(value);
}

#if 0

/*
 * Output the field into the report.
 */

static void hid_output_field(const struct hid_device *hid,
			     struct hid_field *field, __u8 *data)
{
	unsigned count = field->report_count;
	unsigned offset = field->report_offset;
	unsigned size = field->report_size;
	unsigned n;

	for (n = 0; n < count; n++) {
		if (field->logical_minimum < 0)	/* signed values */
			implement(hid, data, offset + n * size, size,
				  s32ton(field->value[n], size));
		else				/* unsigned values */
			implement(hid, data, offset + n * size, size,
				  field->value[n]);
	}
}

/*
 * Create a report. 'data' has to be allocated using
 * hid_alloc_report_buf() so that it has proper size.
 */

void hid_output_report(struct hid_report *report, __u8 *data)
{
	unsigned n;

	if (report->id > 0)
		*data++ = report->id;

	memset(data, 0, ((report->size - 1) >> 3) + 1);
	for (n = 0; n < report->maxfield; n++)
		hid_output_field(report->device, report->field[n], data);
}

/*
 * Allocator for buffer that is going to be passed to hid_output_report()
 */
u8 *hid_alloc_report_buf(struct hid_report *report, gfp_t flags)
{
	/*
	 * 7 extra bytes are necessary to achieve proper functionality
	 * of implement() working on 8 byte chunks
	 */

	int len = ((report->size - 1) >> 3) + 1 + (report->id > 0) + 7;

	return kmalloc(len, flags);
}

/*
 * Set a field value. The report this field belongs to has to be
 * created and transferred to the device, to set this value in the
 * device.
 */

int hid_set_field(struct hid_field *field, unsigned offset, __s32 value)
{
	unsigned size;

	if (!field)
		return -1;

	size = field->report_size;

	hid_dump_input(field->report->device, field->usage + offset, value);

	if (offset >= field->report_count) {
		hid_err(field->report->device, "offset (%d) exceeds report_count (%d)\n",
				offset, field->report_count);
		return -1;
	}
	if (field->logical_minimum < 0) {
		if (value != snto32(s32ton(value, size), size)) {
			hid_err(field->report->device, "value %d is out of range\n", value);
			return -1;
		}
	}
	field->value[offset] = value;
	return 0;
}
EXPORT_SYMBOL_GPL(hid_set_field);

#endif

//static struct hid_report *hid_get_report(struct hid_report_enum *report_enum,
//		const u8 *data)
static struct hid_report *hid_get_report(struct hid_report_enum *report_enum,
    const uint8_t *data)
{
  struct hid_report *report;
  unsigned int n = 0; /* Normally report number is 0 */

  /* Device uses numbered reports, data[0] is report number */
  if (report_enum->numbered)
    n = *data;

  report = report_enum->report_id_hash[n];
  if (report == NULL)
  {
    // dbg_hid("undefined report_id %u received\n", n);
    USBH_UsrLog("undefined report_id %u received", n);
  }

  return report;
}

/*
 * This function is
 * int hid_report_raw_event(struct hid_device *hid, int type, u8 *data, int size, int interrupt)
 */
int hid_report_raw_event(struct hid_device *hid, int type, uint8_t *data, int size)
{
  struct hid_report_enum *report_enum = hid->report_enum + type;
  struct hid_report *report;
  unsigned int a;
  int rsize, csize = size;
  uint8_t *cdata = data;
  int ret = 0;

  USBH_UsrLog("%s", __func__);

  /** these validations are copied from original hid_input_report() **/
  if ((!hid) || (!data) || (!size)) {
    USBH_UsrLog("%s invalid args", __func__);
    return -1;
  }

  /** fetch report **/
  report = hid_get_report(report_enum, data);
  if (!report)
  {
    USBH_UsrLog("failed to get report.")
    goto out;
  }

  /** bypass report id **/
  if (report_enum->numbered)
  {
    cdata++;
    csize--;
  }

  rsize = ((report->size - 1) >> 3) + 1;

  if (rsize > HID_MAX_BUFFER_SIZE)
    rsize = HID_MAX_BUFFER_SIZE;

  if (csize < rsize)
  {
    dbg_hid("report %d is too short, (%d < %d)\n", report->id, csize, rsize);
    memset(cdata + csize, 0, rsize - csize);
  }

//  bypass hiddev report event
//	if ((hid->claimed & HID_CLAIMED_HIDDEV) && hid->hiddev_report_event)
//		hid->hiddev_report_event(hid, report);
//  bypass hidraw report event
//	if (hid->claimed & HID_CLAIMED_HIDRAW) {
//		ret = hidraw_report_event(hid, data, size);
//		if (ret)
//			goto out;
//	}
//  TODO bypass all
//	if (hid->claimed != HID_CLAIMED_HIDRAW && report->maxfield) {
//		for (a = 0; a < report->maxfield; a++)
//			hid_input_field(hid, report->field[a], cdata, interrupt);
//		hdrv = hid->driver;
//		if (hdrv && hdrv->report)
//			hdrv->report(hid, report);
//	}
//

//	USBH_UsrLog("  report is ready, has %d fields.", (int)report->maxfield);
  for (a = 0; a < report->maxfield; a++)
  {
//  USBH_UsrLog( "  field index: %d", a);
    hid_input_field(hid, report->field[a], cdata);
  }

//  if (hid->claimed & HID_CLAIMED_INPUT)
  hidinput_report_event(hid, report);

  out: return ret;
}
// EXPORT_SYMBOL_GPL(hid_report_raw_event);


/**
 * hid_input_report - report data from lower layer (usb, bt...)
 *
 * @hid: hid device
 * @type: HID report type (HID_*_REPORT)
 * @data: report contents
 * @size: size of data parameter
 * @interrupt: distinguish between interrupt and control transfers
 *
 * This is data entry for lower layers.
 *
 * int hid_input_report(struct hid_device *hid, int type, u8 *data, int size, int interrupt)
 */
//int hid_input_report(struct hid_device *hid, int type, uint8_t *data, int size,
//    int interrupt)
//{
////  struct hid_report_enum *report_enum;
////	struct hid_driver *hdrv;
////  struct hid_report *report;
//  int ret = 0;
//
//  if (!hid)
//    return -ENODEV;
//
////	if (down_trylock(&hid->driver_input_lock))
////		return -EBUSY;
//
////	if (!hid->driver) {
////		ret = -ENODEV;
////		goto unlock;
////	}
////  report_enum = hid->report_enum + type;
////	hdrv = hid->driver;
//
//  if (!size)
//  {
////	dbg_hid("empty report\n");
//    USBH_UsrLog("empty report");
//    ret = -1;
//    goto unlock;
//  }
//
////  /* Avoid unnecessary overhead if debugfs is disabled */
////	if (!list_empty(&hid->debug_list))
////		hid_dump_report(hid, type, data, size);
////  These code are duplicate. They are called in hid_report_raw_event
////	report = hid_get_report(report_enum, data);
////
////	if (!report) {
////		ret = -1;
////		goto unlock;
////	}
////  bypass driver's raw_event handler
////	if (hdrv && hdrv->raw_event && hid_match_report(hid, report)) {
////		ret = hdrv->raw_event(hid, report, data, size);
////		if (ret < 0)
////			goto unlock;
////	}
//  ret = hid_report_raw_event(hid, type, data, size, interrupt);
//
//  unlock:
////	up(&hid->driver_input_lock);
//  return ret;
//}
// EXPORT_SYMBOL_GPL(hid_input_report);

#if 0

static bool hid_match_one_id(struct hid_device *hdev,
		const struct hid_device_id *id)
{
	return (id->bus == HID_BUS_ANY || id->bus == hdev->bus) &&
		(id->group == HID_GROUP_ANY || id->group == hdev->group) &&
		(id->vendor == HID_ANY_ID || id->vendor == hdev->vendor) &&
		(id->product == HID_ANY_ID || id->product == hdev->product);
}

const struct hid_device_id *hid_match_id(struct hid_device *hdev,
		const struct hid_device_id *id)
{
	for (; id->bus; id++)
		if (hid_match_one_id(hdev, id))
			return id;

	return NULL;
}

static const struct hid_device_id hid_hiddev_list[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_MGE, USB_DEVICE_ID_MGE_UPS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MGE, USB_DEVICE_ID_MGE_UPS1) },
	{ }
};

static bool hid_hiddev(struct hid_device *hdev)
{
	return !!hid_match_id(hdev, hid_hiddev_list);
}


static ssize_t
read_report_descriptor(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr,
		char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);

	if (off >= hdev->rsize)
		return 0;

	if (off + count > hdev->rsize)
		count = hdev->rsize - off;

	memcpy(buf, hdev->rdesc + off, count);

	return count;
}

static struct bin_attribute dev_bin_attr_report_desc = {
	.attr = { .name = "report_descriptor", .mode = 0444 },
	.read = read_report_descriptor,
	.size = HID_MAX_DESCRIPTOR_SIZE,
};

int hid_connect(struct hid_device *hdev, unsigned int connect_mask)
{
    static const char *types[] = { "Device", "Pointer", "Mouse", "Device",
        "Joystick", "Gamepad", "Keyboard", "Keypad",
        "Multi-Axis Controller"
    };
    const char *type, *bus;
    char buf[64];
    unsigned int i;
    int len;
    int ret;

    if (hdev->quirks & HID_QUIRK_HIDDEV_FORCE)
        connect_mask |= (HID_CONNECT_HIDDEV_FORCE | HID_CONNECT_HIDDEV);
    if (hdev->quirks & HID_QUIRK_HIDINPUT_FORCE)
        connect_mask |= HID_CONNECT_HIDINPUT_FORCE;
    if (hdev->bus != BUS_USB)
        connect_mask &= ~HID_CONNECT_HIDDEV;
    if (hid_hiddev(hdev))
        connect_mask |= HID_CONNECT_HIDDEV_FORCE;

    if ((connect_mask & HID_CONNECT_HIDINPUT) && !hidinput_connect(hdev,
                connect_mask & HID_CONNECT_HIDINPUT_FORCE))
        hdev->claimed |= HID_CLAIMED_INPUT;

    if ((connect_mask & HID_CONNECT_HIDDEV) && hdev->hiddev_connect &&
            !hdev->hiddev_connect(hdev,
                connect_mask & HID_CONNECT_HIDDEV_FORCE))
        hdev->claimed |= HID_CLAIMED_HIDDEV;
    if ((connect_mask & HID_CONNECT_HIDRAW) && !hidraw_connect(hdev))
        hdev->claimed |= HID_CLAIMED_HIDRAW;

    /* Drivers with the ->raw_event callback set are not required to connect
     * to any other listener. */
    if (!hdev->claimed && !hdev->driver->raw_event) {
        hid_err(hdev, "device has no listeners, quitting\n");
        return -ENODEV;
    }

    if ((hdev->claimed & HID_CLAIMED_INPUT) &&
            (connect_mask & HID_CONNECT_FF) && hdev->ff_init)
        hdev->ff_init(hdev);

    len = 0;
    if (hdev->claimed & HID_CLAIMED_INPUT)
        len += sprintf(buf + len, "input");
    if (hdev->claimed & HID_CLAIMED_HIDDEV)
        len += sprintf(buf + len, "%shiddev%d", len ? "," : "",
                hdev->minor);
    if (hdev->claimed & HID_CLAIMED_HIDRAW)
        len += sprintf(buf + len, "%shidraw%d", len ? "," : "",
                ((struct hidraw *)hdev->hidraw)->minor);

    type = "Device";
    for (i = 0; i < hdev->maxcollection; i++) {
        struct hid_collection *col = &hdev->collection[i];
        if (col->type == HID_COLLECTION_APPLICATION &&
           (col->usage & HID_USAGE_PAGE) == HID_UP_GENDESK &&
           (col->usage & 0xffff) < ARRAY_SIZE(types)) {
            type = types[col->usage & 0xffff];
            break;
        }
    }

    switch (hdev->bus) {
    case BUS_USB:
        bus = "USB";
        break;
    case BUS_BLUETOOTH:
        bus = "BLUETOOTH";
        break;
    default:
        bus = "<UNKNOWN>";
    }

    ret = device_create_bin_file(&hdev->dev, &dev_bin_attr_report_desc);
    if (ret)
        hid_warn(hdev,
             "can't create sysfs report descriptor attribute err: %d\n", ret);

    hid_info(hdev, "%s: %s HID v%x.%02x %s [%s] on %s\n",
         buf, bus, hdev->version >> 8, hdev->version & 0xff,
         type, hdev->name, hdev->phys);

    return 0;
}

void hid_disconnect(struct hid_device *hdev)
{
    device_remove_bin_file(&hdev->dev, &dev_bin_attr_report_desc);
    if (hdev->claimed & HID_CLAIMED_INPUT)
        hidinput_disconnect(hdev);
    if (hdev->claimed & HID_CLAIMED_HIDDEV)
        hdev->hiddev_disconnect(hdev);
    if (hdev->claimed & HID_CLAIMED_HIDRAW)
        hidraw_disconnect(hdev);
}

/*
 * A list of devices for which there is a specialized driver on HID bus.
 *
 * Please note that for multitouch devices (driven by hid-multitouch driver),
 * there is a proper autodetection and autoloading in place (based on presence
 * of HID_DG_CONTACTID), so those devices don't need to be added to this list,
 * as we are doing the right thing in hid_scan_usage().
 *
 * Autodetection for (USB) HID sensor hubs exists too. If a collection of type
 * physical is found inside a usage page of type sensor, hid-sensor-hub will be
 * used as a driver. See hid_scan_report().
 */
static const struct hid_device_id hid_have_special_driver[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_A4TECH, USB_DEVICE_ID_A4TECH_WCP32PU) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_A4TECH, USB_DEVICE_ID_A4TECH_X5_005D) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_A4TECH, USB_DEVICE_ID_A4TECH_RP_649) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ACRUX, 0x0802) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ACRUX, 0xf705) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MIGHTYMOUSE) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MAGICMOUSE) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_MAGICTRACKPAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_MINI_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_MINI_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_MINI_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_HF_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_HF_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_HF_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IRCONTROL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IRCONTROL2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IRCONTROL3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IRCONTROL4) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_IRCONTROL5) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_ANSI) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_ISO) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING2_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING2_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING2_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING3_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING3_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING3_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_REVB_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_REVB_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_REVB_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING8_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING8_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING8_JIS) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_2009_ANSI) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_2009_ISO) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_2009_JIS) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_2011_ANSI) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_2011_ISO) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_ALU_WIRELESS_2011_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_TP_ONLY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER1_TP_ONLY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AUREAL, USB_DEVICE_ID_AUREAL_W01RN) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_BELKIN, USB_DEVICE_ID_FLIP_KVM) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_BTC, USB_DEVICE_ID_BTC_EMPREX_REMOTE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_BTC, USB_DEVICE_ID_BTC_EMPREX_REMOTE_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHERRY, USB_DEVICE_ID_CHERRY_CYMOTION) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHERRY, USB_DEVICE_ID_CHERRY_CYMOTION_SOLAR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHICONY, USB_DEVICE_ID_CHICONY_TACTICAL_PAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHICONY, USB_DEVICE_ID_CHICONY_WIRELESS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHICONY, USB_DEVICE_ID_CHICONY_WIRELESS2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHICONY, USB_DEVICE_ID_CHICONY_AK1D) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CREATIVELABS, USB_DEVICE_ID_PRODIKEYS_PCMIDI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_BARCODE_1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_BARCODE_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_BARCODE_3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_BARCODE_4) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_MOUSE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DRAGONRISE, 0x0006) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DRAGONRISE, 0x0011) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_ELECOM, USB_DEVICE_ID_ELECOM_BM084) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ELO, 0x0009) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ELO, 0x0030) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_EMS, USB_DEVICE_ID_EMS_TRIO_LINKER_PLUS_II) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_EZKEY, USB_DEVICE_ID_BTC_8193) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GAMERON, USB_DEVICE_ID_GAMERON_DUAL_PSX_ADAPTOR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GAMERON, USB_DEVICE_ID_GAMERON_DUAL_PCS_ADAPTOR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GREENASIA, 0x0003) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GREENASIA, 0x0012) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GYRATION, USB_DEVICE_ID_GYRATION_REMOTE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GYRATION, USB_DEVICE_ID_GYRATION_REMOTE_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GYRATION, USB_DEVICE_ID_GYRATION_REMOTE_3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK, USB_DEVICE_ID_HOLTEK_ON_LINE_GRIP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK_ALT, USB_DEVICE_ID_HOLTEK_ALT_KEYBOARD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK_ALT, USB_DEVICE_ID_HOLTEK_ALT_MOUSE_A04A) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK_ALT, USB_DEVICE_ID_HOLTEK_ALT_MOUSE_A067) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK_ALT, USB_DEVICE_ID_HOLTEK_ALT_MOUSE_A070) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK_ALT, USB_DEVICE_ID_HOLTEK_ALT_MOUSE_A072) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HOLTEK_ALT, USB_DEVICE_ID_HOLTEK_ALT_MOUSE_A081) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_HUION, USB_DEVICE_ID_HUION_580) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_JESS2, USB_DEVICE_ID_JESS2_COLOR_RUMBLE_PAD) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_ION, USB_DEVICE_ID_ICADE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KENSINGTON, USB_DEVICE_ID_KS_SLIMBLADE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KEYTOUCH, USB_DEVICE_ID_KEYTOUCH_IEC) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_GENIUS_GILA_GAMING_MOUSE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_GENIUS_MANTICORE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_GENIUS_GX_IMPERATOR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_KYE_ERGO_525V) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_KYE_EASYPEN_I405X) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_KYE_MOUSEPEN_I608X) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_KYE_EASYPEN_M610X) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LABTEC, USB_DEVICE_ID_LABTEC_WIRELESS_KEYBOARD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LCPOWER, USB_DEVICE_ID_LCPOWER_LC1000 ) },
#if IS_ENABLED(CONFIG_HID_LENOVO_TPKBD)
	{ HID_USB_DEVICE(USB_VENDOR_ID_LENOVO, USB_DEVICE_ID_LENOVO_TPKBD) },
#endif
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_MX3000_RECEIVER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_S510_RECEIVER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_S510_RECEIVER_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_RECEIVER) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_HARMONY_PS3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_DINOVO_DESKTOP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_DINOVO_EDGE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_DINOVO_MINI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_ELITE_KBD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_CORDLESS_DESKTOP_LX500) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_EXTREME_3D) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_RUMBLEPAD_CORD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_RUMBLEPAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_RUMBLEPAD2_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_WINGMAN_F3D) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_WINGMAN_FFG ) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_FORCE3D_PRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_FLIGHT_SYSTEM_G940) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_MOMO_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_MOMO_WHEEL2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_VIBRATION_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_DFP_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_DFGT_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G25_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G27_WHEEL) },
#if IS_ENABLED(CONFIG_HID_LOGITECH_DJ)
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_UNIFYING_RECEIVER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_UNIFYING_RECEIVER_2) },
#endif
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_WII_WHEEL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_RUMBLEPAD2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_SPACETRAVELLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_SPACENAVIGATOR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROCHIP, USB_DEVICE_ID_PICOLCD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROCHIP, USB_DEVICE_ID_PICOLCD_BOOTLOADER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_COMFORT_MOUSE_4500) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_SIDEWINDER_GV) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_NE4K) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_NE4K_JP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_LK6K) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_PRESENTER_8K_USB) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_DIGITAL_MEDIA_3K) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_WIRELESS_OPTICAL_DESKTOP_3_0) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MONTEREY, USB_DEVICE_ID_GENIUS_KB29E) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_4) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_5) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_6) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_7) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_8) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_9) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_10) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_11) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_12) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_13) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_14) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_15) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_16) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_17) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NTRIG, USB_DEVICE_ID_NTRIG_TOUCH_SCREEN_18) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ORTEK, USB_DEVICE_ID_ORTEK_PKB1700) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ORTEK, USB_DEVICE_ID_ORTEK_WKB2000) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PETALYNX, USB_DEVICE_ID_PETALYNX_MAXTER_REMOTE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PRIMAX, USB_DEVICE_ID_PRIMAX_KEYBOARD) },
#if IS_ENABLED(CONFIG_HID_ROCCAT)
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_ARVO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_ISKU) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_ISKUFX) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_KONE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_KONEPLUS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_KONEPURE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_KONEPURE_OPTICAL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_KONEXTD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_KOVAPLUS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_LUA) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_PYRA_WIRED) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_PYRA_WIRELESS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_RYOS_MK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_RYOS_MK_GLOW) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_RYOS_MK_PRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ROCCAT, USB_DEVICE_ID_ROCCAT_SAVU) },
#endif
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAITEK, USB_DEVICE_ID_SAITEK_PS1000) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG, USB_DEVICE_ID_SAMSUNG_IR_REMOTE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG, USB_DEVICE_ID_SAMSUNG_WIRELESS_KBD_MOUSE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SKYCABLE, USB_DEVICE_ID_SKYCABLE_WIRELESS_PRESENTER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_BUZZ_CONTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_WIRELESS_BUZZ_CONTROLLER) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_PS3_BDREMOTE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_PS3_CONTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_NAVIGATION_CONTROLLER) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_PS3_CONTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_PS4_CONTROLLER) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_PS4_CONTROLLER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_VAIO_VGX_MOUSE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SONY, USB_DEVICE_ID_SONY_VAIO_VGP_MOUSE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES, USB_DEVICE_ID_STEELSERIES_SRWS1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SUNPLUS, USB_DEVICE_ID_SUNPLUS_WDESKTOP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THINGM, USB_DEVICE_ID_BLINK1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb300) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb304) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb323) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb324) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb651) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb653) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb654) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb65a) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_TIVO, USB_DEVICE_ID_TIVO_SLIDE_BT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_TIVO, USB_DEVICE_ID_TIVO_SLIDE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_TOPSEED, USB_DEVICE_ID_TOPSEED_CYBERLINK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_TOPSEED2, USB_DEVICE_ID_TOPSEED2_RF_COMBO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_TWINHAN, USB_DEVICE_ID_TWINHAN_IR_REMOTE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_TABLET_PF1209) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_TABLET_WP4030U) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_TABLET_WP5540U) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_TABLET_WP8060U) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_TABLET_WP1062) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_WIRELESS_TABLET_TWHL850) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_UCLOGIC, USB_DEVICE_ID_UCLOGIC_TABLET_TWHA60) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_SMARTJOY_PLUS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_SUPER_JOY_BOX_3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_DUAL_USB_JOYPAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP_LTD, USB_DEVICE_ID_SUPER_JOY_BOX_3_PRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP_LTD, USB_DEVICE_ID_SUPER_DUAL_BOX_PRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP_LTD, USB_DEVICE_ID_SUPER_JOY_BOX_5_PRO) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_GRAPHIRE_BLUETOOTH) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_WACOM, USB_DEVICE_ID_WACOM_INTUOS4_BLUETOOTH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_SLIM_TABLET_5_8_INCH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_SLIM_TABLET_12_1_INCH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_Q_PAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_PID_0038) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_MEDIA_TABLET_10_6_INCH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_MEDIA_TABLET_14_1_INCH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WALTOP, USB_DEVICE_ID_WALTOP_SIRIUS_BATTERY_FREE_TABLET) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_X_TENSIONS, USB_DEVICE_ID_SPEEDLINK_VAD_CEZANNE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_XIN_MO, USB_DEVICE_ID_XIN_MO_DUAL_ARCADE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ZEROPLUS, 0x0005) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ZEROPLUS, 0x0030) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ZYDACRON, USB_DEVICE_ID_ZYDACRON_REMOTE_CONTROL) },

	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_MICROSOFT, USB_DEVICE_ID_MS_PRESENTER_8K_BT) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_WIIMOTE) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_WIIMOTE2) },
	{ }
};

struct hid_dynid {
	struct list_head list;
	struct hid_device_id id;
};

/**
 * store_new_id - add a new HID device ID to this driver and re-probe devices
 * @driver: target device driver
 * @buf: buffer for scanning device ID data
 * @count: input size
 *
 * Adds a new dynamic hid device ID to this driver,
 * and causes the driver to probe for all devices again.
 */
static ssize_t store_new_id(struct device_driver *drv, const char *buf,
		size_t count)
{
	struct hid_driver *hdrv = container_of(drv, struct hid_driver, driver);
	struct hid_dynid *dynid;
	__u32 bus, vendor, product;
	unsigned long driver_data = 0;
	int ret;

	ret = sscanf(buf, "%x %x %x %lx",
			&bus, &vendor, &product, &driver_data);
	if (ret < 3)
		return -EINVAL;

	dynid = kzalloc(sizeof(*dynid), GFP_KERNEL);
	if (!dynid)
		return -ENOMEM;

	dynid->id.bus = bus;
	dynid->id.group = HID_GROUP_ANY;
	dynid->id.vendor = vendor;
	dynid->id.product = product;
	dynid->id.driver_data = driver_data;

	spin_lock(&hdrv->dyn_lock);
	list_add_tail(&dynid->list, &hdrv->dyn_list);
	spin_unlock(&hdrv->dyn_lock);

	ret = driver_attach(&hdrv->driver);

	return ret ? : count;
}
static DRIVER_ATTR(new_id, S_IWUSR, NULL, store_new_id);

static void hid_free_dynids(struct hid_driver *hdrv)
{
	struct hid_dynid *dynid, *n;

	spin_lock(&hdrv->dyn_lock);
	list_for_each_entry_safe(dynid, n, &hdrv->dyn_list, list) {
		list_del(&dynid->list);
		kfree(dynid);
	}
	spin_unlock(&hdrv->dyn_lock);
}

static const struct hid_device_id *hid_match_device(struct hid_device *hdev,
		struct hid_driver *hdrv)
{
	struct hid_dynid *dynid;

	spin_lock(&hdrv->dyn_lock);
	list_for_each_entry(dynid, &hdrv->dyn_list, list) {
		if (hid_match_one_id(hdev, &dynid->id)) {
			spin_unlock(&hdrv->dyn_lock);
			return &dynid->id;
		}
	}
	spin_unlock(&hdrv->dyn_lock);

	return hid_match_id(hdev, hdrv->id_table);
}

static int hid_bus_match(struct device *dev, struct device_driver *drv)
{
	struct hid_driver *hdrv = container_of(drv, struct hid_driver, driver);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);

	return hid_match_device(hdev, hdrv) != NULL;
}

static int hid_device_probe(struct device *dev)
{
  struct hid_driver *hdrv = container_of(dev->driver, struct hid_driver,
      driver);
  struct hid_device *hdev = container_of(dev, struct hid_device, dev);
  const struct hid_device_id *id;
  int ret = 0;

  if (down_interruptible(&hdev->driver_lock))
    return -EINTR;
  if (down_interruptible(&hdev->driver_input_lock))
  {
    ret = -EINTR;
    goto unlock_driver_lock;
  }
  hdev->io_started = false;

  if (!hdev->driver)
  {
    id = hid_match_device(hdev, hdrv);
    if (id == NULL)
    {
      ret = -ENODEV;
      goto unlock;
    }

    hdev->driver = hdrv;
    if (hdrv->probe)
    {
      ret = hdrv->probe(hdev, id);
    }
    else
    { /* default probe */
      ret = hid_open_report(hdev);
      if (!ret)
        ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    }
    if (ret)
    {
      hid_close_report(hdev);
      hdev->driver = NULL;
    }
  }
  unlock: if (!hdev->io_started)
    up(&hdev->driver_input_lock);
  unlock_driver_lock: up(&hdev->driver_lock);
  return ret;
}

static int hid_device_remove(struct device *dev)
{
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct hid_driver *hdrv;
	int ret = 0;

	if (down_interruptible(&hdev->driver_lock))
		return -EINTR;
	if (down_interruptible(&hdev->driver_input_lock)) {
		ret = -EINTR;
		goto unlock_driver_lock;
	}
	hdev->io_started = false;

	hdrv = hdev->driver;
	if (hdrv) {
		if (hdrv->remove)
			hdrv->remove(hdev);
		else /* default remove */
			hid_hw_stop(hdev);
		hid_close_report(hdev);
		hdev->driver = NULL;
	}

	if (!hdev->io_started)
		up(&hdev->driver_input_lock);
unlock_driver_lock:
	up(&hdev->driver_lock);
	return ret;
}

static ssize_t modalias_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	int len;

	len = snprintf(buf, PAGE_SIZE, "hid:b%04Xg%04Xv%08Xp%08X\n",
		       hdev->bus, hdev->group, hdev->vendor, hdev->product);

	return (len >= PAGE_SIZE) ? (PAGE_SIZE - 1) : len;
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *hid_dev_attrs[] = {
	&dev_attr_modalias.attr,
	NULL,
};
ATTRIBUTE_GROUPS(hid_dev);

static int hid_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);

	if (add_uevent_var(env, "HID_ID=%04X:%08X:%08X",
			hdev->bus, hdev->vendor, hdev->product))
		return -ENOMEM;

	if (add_uevent_var(env, "HID_NAME=%s", hdev->name))
		return -ENOMEM;

	if (add_uevent_var(env, "HID_PHYS=%s", hdev->phys))
		return -ENOMEM;

	if (add_uevent_var(env, "HID_UNIQ=%s", hdev->uniq))
		return -ENOMEM;

	if (add_uevent_var(env, "MODALIAS=hid:b%04Xg%04Xv%08Xp%08X",
			   hdev->bus, hdev->group, hdev->vendor, hdev->product))
		return -ENOMEM;

	return 0;
}

static struct bus_type hid_bus_type = {
	.name		= "hid",
	.dev_groups	= hid_dev_groups,
	.match		= hid_bus_match,
	.probe		= hid_device_probe,
	.remove		= hid_device_remove,
	.uevent		= hid_uevent,
};

/* a list of devices that shouldn't be handled by HID core at all */
static const struct hid_device_id hid_ignore_list[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_ACECAD, USB_DEVICE_ID_ACECAD_FLAIR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ACECAD, USB_DEVICE_ID_ACECAD_302) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ADS_TECH, USB_DEVICE_ID_ADS_TECH_RADIO_SI470X) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_01) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_10) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_20) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_21) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_22) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_23) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIPTEK, USB_DEVICE_ID_AIPTEK_24) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AIRCABLE, USB_DEVICE_ID_AIRCABLE1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ALCOR, USB_DEVICE_ID_ALCOR_USBRS232) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_LCM)},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_LCM2)},
	{ HID_USB_DEVICE(USB_VENDOR_ID_AVERMEDIA, USB_DEVICE_ID_AVER_FM_MR800) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_AXENTIA, USB_DEVICE_ID_AXENTIA_FM_RADIO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_BERKSHIRE, USB_DEVICE_ID_BERKSHIRE_PCWD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CIDC, 0x0103) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYGNAL, USB_DEVICE_ID_CYGNAL_RADIO_SI470X) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYGNAL, USB_DEVICE_ID_CYGNAL_RADIO_SI4713) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CMEDIA, USB_DEVICE_ID_CM109) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_HIDCOM) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CYPRESS, USB_DEVICE_ID_CYPRESS_ULTRAMOUSE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DEALEXTREAME, USB_DEVICE_ID_DEALEXTREAME_RADIO_SI4701) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DELORME, USB_DEVICE_ID_DELORME_EARTHMATE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DELORME, USB_DEVICE_ID_DELORME_EM_LT20) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DREAM_CHEEKY, 0x0004) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DREAM_CHEEKY, 0x000a) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ESSENTIAL_REALITY, USB_DEVICE_ID_ESSENTIAL_REALITY_P5) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ETT, USB_DEVICE_ID_TC5UH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ETT, USB_DEVICE_ID_TC4UM) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH, 0x0001) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH, 0x0002) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GENERAL_TOUCH, 0x0004) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_4_PHIDGETSERVO_30) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_1_PHIDGETSERVO_30) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_0_4_IF_KIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_16_16_IF_KIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_8_8_8_IF_KIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_8_7_IF_KIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_0_8_8_IF_KIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GLAB, USB_DEVICE_ID_PHIDGET_MOTORCONTROL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GOTOP, USB_DEVICE_ID_SUPER_Q2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GOTOP, USB_DEVICE_ID_GOGOPEN) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GOTOP, USB_DEVICE_ID_PENPOWER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GRETAGMACBETH, USB_DEVICE_ID_GRETAGMACBETH_HUEY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_POWERMATE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_SOUNDKNOB) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GRIFFIN, USB_DEVICE_ID_RADIOSHARK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_90) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_100) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_101) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_103) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_104) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_105) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_106) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_107) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_108) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_200) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_201) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_202) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_203) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_204) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_205) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_206) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_207) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_300) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_301) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_302) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_303) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_304) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_305) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_306) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_307) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_308) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_309) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_400) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_401) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_402) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_403) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_404) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_405) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_500) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_501) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_502) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_503) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_504) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1000) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1001) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1002) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1003) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1004) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1005) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1006) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_GTCO, USB_DEVICE_ID_GTCO_1007) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_IMATION, USB_DEVICE_ID_DISC_STAKKA) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_JABRA, USB_DEVICE_ID_JABRA_SPEAK_410) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_JABRA, USB_DEVICE_ID_JABRA_SPEAK_510) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KBGEAR, USB_DEVICE_ID_KBGEAR_JAMSTUDIO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KWORLD, USB_DEVICE_ID_KWORLD_RADIO_FM700) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_KYE, USB_DEVICE_ID_KYE_GPEN_560) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_KYE, 0x0058) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_CASSY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_CASSY2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_POCKETCASSY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_POCKETCASSY2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MOBILECASSY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MOBILECASSY2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MICROCASSYVOLTAGE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MICROCASSYCURRENT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MICROCASSYTIME) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MICROCASSYTEMPERATURE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MICROCASSYPH) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_JWM) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_DMMP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_UMIP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_UMIC) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_UMIB) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_XRAY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_XRAY2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_VIDEOCOM) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MOTOR) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_COM3LAB) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_TELEPORT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_NETWORKANALYSER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_POWERCONTROL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MACHINETEST) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MOSTANALYSER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MOSTANALYSER2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_ABSESP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_AUTODATABUS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_MCT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_HYBRID) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LD, USB_DEVICE_ID_LD_HEATCONTROL) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MADCATZ, USB_DEVICE_ID_MADCATZ_BEATPAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MCC, USB_DEVICE_ID_MCC_PMD1024LS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MCC, USB_DEVICE_ID_MCC_PMD1208LS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROCHIP, USB_DEVICE_ID_PICKIT1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_MICROCHIP, USB_DEVICE_ID_PICKIT2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NATIONAL_SEMICONDUCTOR, USB_DEVICE_ID_N_S_HARMONY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 20) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 30) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 100) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 108) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 118) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 200) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 300) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 400) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ONTRAK, USB_DEVICE_ID_ONTRAK_ADU100 + 500) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PANJIT, 0x0001) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PANJIT, 0x0002) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PANJIT, 0x0003) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PANJIT, 0x0004) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_PHILIPS, USB_DEVICE_ID_PHILIPS_IEEE802154_DONGLE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_POWERCOM, USB_DEVICE_ID_POWERCOM_UPS) },
#if defined(CONFIG_MOUSE_SYNAPTICS_USB) || defined(CONFIG_MOUSE_SYNAPTICS_USB_MODULE)
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_TP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_INT_TP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_CPAD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_STICK) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_WP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_COMP_TP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_WTP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SYNAPTICS, USB_DEVICE_ID_SYNAPTICS_DPAD) },
#endif
	{ HID_USB_DEVICE(USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_LABPRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_GOTEMP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_SKIP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_CYCLOPS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_VERNIER, USB_DEVICE_ID_VERNIER_LCSPEC) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WACOM, HID_ANY_ID) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_4_PHIDGETSERVO_20) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_1_PHIDGETSERVO_20) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WISEGROUP, USB_DEVICE_ID_8_8_4_IF_KIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_YEALINK, USB_DEVICE_ID_YEALINK_P1K_P4K_B2K) },
	{ }
};

/**
 * hid_mouse_ignore_list - mouse devices which should not be handled by the hid layer
 *
 * There are composite devices for which we want to ignore only a certain
 * interface. This is a list of devices for which only the mouse interface will
 * be ignored. This allows a dedicated driver to take care of the interface.
 */
static const struct hid_device_id hid_mouse_ignore_list[] = {
	/* appletouch driver */
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER3_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_HF_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_HF_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER4_HF_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING2_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING2_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING2_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING3_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING3_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING3_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING4A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING5A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING6A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7A_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7A_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING7A_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING8_ANSI) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING8_ISO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_WELLSPRING8_JIS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_FOUNTAIN_TP_ONLY) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE, USB_DEVICE_ID_APPLE_GEYSER1_TP_ONLY) },
	{ }
};

bool hid_ignore(struct hid_device *hdev)
{
	if (hdev->quirks & HID_QUIRK_NO_IGNORE)
		return false;
	if (hdev->quirks & HID_QUIRK_IGNORE)
		return true;

	switch (hdev->vendor) {
	case USB_VENDOR_ID_CODEMERCS:
		/* ignore all Code Mercenaries IOWarrior devices */
		if (hdev->product >= USB_DEVICE_ID_CODEMERCS_IOW_FIRST &&
				hdev->product <= USB_DEVICE_ID_CODEMERCS_IOW_LAST)
			return true;
		break;
	case USB_VENDOR_ID_LOGITECH:
		if (hdev->product >= USB_DEVICE_ID_LOGITECH_HARMONY_FIRST &&
				hdev->product <= USB_DEVICE_ID_LOGITECH_HARMONY_LAST)
			return true;
		/*
		 * The Keene FM transmitter USB device has the same USB ID as
		 * the Logitech AudioHub Speaker, but it should ignore the hid.
		 * Check if the name is that of the Keene device.
		 * For reference: the name of the AudioHub is
		 * "HOLTEK  AudioHub Speaker".
		 */
		if (hdev->product == USB_DEVICE_ID_LOGITECH_AUDIOHUB &&
			!strcmp(hdev->name, "HOLTEK  B-LINK USB Audio  "))
				return true;
		break;
	case USB_VENDOR_ID_SOUNDGRAPH:
		if (hdev->product >= USB_DEVICE_ID_SOUNDGRAPH_IMON_FIRST &&
		    hdev->product <= USB_DEVICE_ID_SOUNDGRAPH_IMON_LAST)
			return true;
		break;
	case USB_VENDOR_ID_HANWANG:
		if (hdev->product >= USB_DEVICE_ID_HANWANG_TABLET_FIRST &&
		    hdev->product <= USB_DEVICE_ID_HANWANG_TABLET_LAST)
			return true;
		break;
	case USB_VENDOR_ID_JESS:
		if (hdev->product == USB_DEVICE_ID_JESS_YUREX &&
				hdev->type == HID_TYPE_USBNONE)
			return true;
		break;
	case USB_VENDOR_ID_VELLEMAN:
		/* These are not HID devices.  They are handled by comedi. */
		if ((hdev->product >= USB_DEVICE_ID_VELLEMAN_K8055_FIRST &&
		     hdev->product <= USB_DEVICE_ID_VELLEMAN_K8055_LAST) ||
		    (hdev->product >= USB_DEVICE_ID_VELLEMAN_K8061_FIRST &&
		     hdev->product <= USB_DEVICE_ID_VELLEMAN_K8061_LAST))
			return true;
		break;
	case USB_VENDOR_ID_ATMEL_V_USB:
		/* Masterkit MA901 usb radio based on Atmel tiny85 chip and
		 * it has the same USB ID as many Atmel V-USB devices. This
		 * usb radio is handled by radio-ma901.c driver so we want
		 * ignore the hid. Check the name, bus, product and ignore
		 * if we have MA901 usb radio.
		 */
		if (hdev->product == USB_DEVICE_ID_ATMEL_V_USB &&
			hdev->bus == BUS_USB &&
			strncmp(hdev->name, "www.masterkit.ru MA901", 22) == 0)
			return true;
		break;
	}

	if (hdev->type == HID_TYPE_USBMOUSE &&
			hid_match_id(hdev, hid_mouse_ignore_list))
		return true;

	return !!hid_match_id(hdev, hid_ignore_list);
}

int hid_add_device(struct hid_device *hdev)
{
	static atomic_t id = ATOMIC_INIT(0);
	int ret;

	if (WARN_ON(hdev->status & HID_STAT_ADDED))
		return -EBUSY;

	/* we need to kill them here, otherwise they will stay allocated to
	 * wait for coming driver */
	if (hid_ignore(hdev))
		return -ENODEV;

	/*
	 * Read the device report descriptor once and use as template
	 * for the driver-specific modifications.
	 */
	ret = hdev->ll_driver->parse(hdev);
	if (ret)
		return ret;
	if (!hdev->dev_rdesc)
		return -ENODEV;

	/*
	 * Scan generic devices for group information
	 */
	if (hid_ignore_special_drivers ||
	    !hid_match_id(hdev, hid_have_special_driver)) {
		ret = hid_scan_report(hdev);
		if (ret)
			hid_warn(hdev, "bad device descriptor (%d)\n", ret);
	}

	/* XXX hack, any other cleaner solution after the driver core
	 * is converted to allow more than 20 bytes as the device name? */
	dev_set_name(&hdev->dev, "%04X:%04X:%04X.%04X", hdev->bus,
		     hdev->vendor, hdev->product, atomic_inc_return(&id));

	hid_debug_register(hdev, dev_name(&hdev->dev));
	ret = device_add(&hdev->dev);
	if (!ret)
		hdev->status |= HID_STAT_ADDED;
	else
		hid_debug_unregister(hdev);

	return ret;
}
#endif


/**
 * hid_allocate_device - allocate new hid device descriptor
 *
 * Allocate and initialize hid device, so that hid_destroy_device might be
 * used to free it.
 *
 * New hid_device pointer is returned on success, otherwise ERR_PTR encoded
 * error value.
 */
struct hid_device *hid_request_device(void)
{
  if (hid_device1_requested) {
    return NULL;
  }

  memset(&hid_device1, 0, sizeof(hid_device1));

  return &hid_device1;
}

/**
 * hid_destroy_device - free previously allocated device
 *
 * @hdev: hid device
 *
 * If you allocate hid_device through hid_allocate_device, you should ever
 * free by this function.
 */
void hid_release_device(struct hid_device *hdev)
{
  if (hdev == 0) {
    USBH_UsrLog("hid: invalid params");
    return;
  }

  if (hdev == &hid_device1) {
    if (hid_device1_requested) {
      hid_device1_requested = 0;
    }
    else {
      USBH_UsrLog("hid: release a device that is not requested");
    }
  }
}




