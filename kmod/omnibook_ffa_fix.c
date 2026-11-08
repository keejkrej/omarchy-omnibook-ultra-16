// SPDX-License-Identifier: GPL-2.0
/*
 * HP OmniBook Ultra 16-bk0xxx (NVIDIA N1X / GB10) ACPI FF-A EC & Battery Fix
 *
 * Fixes two DSDT AML bugs on the HP OmniBook Ultra Laptop 16-bk0xxx:
 *
 * 1. \_SB.FFA0.RBUF defines an unused field:
 *      CreateField (BUFF, 0xD0, 0x0190, DATA)
 *    which spans bits 208..608 (76 bytes) on a 64-byte Buffer(0x40), causing
 *    ACPICA to abort every EC read with AE_AML_BUFFER_LIMIT. Clamping the bit
 *    length in RBUF's AML from 0x0190 (400 bits) to 0x0130 (304 bits) keeps
 *    the field within the 512-bit buffer and unblocks BAT0, ADP1, TZ01, FAN0,
 *    USC1, LID0, and EC _DSM notifications.
 *
 * 2. \_SB.BAT0._BST calls SREQ(0x02, HPBM) targeting NVIDIA's reference UUID
 *    25cb5207-ac36-427d-aaef-3aa78877d27e (absent on HP, whose EC partition is
 *    884a63a0-3285-4120-83aa-eec008a0a546) instead of calling HP's own
 *    \_SB.BAT0.UPBS() method which populates PBST via \_SB.FFA0.RBUF.
 *    Redirecting _BST's AML body to `UPBS () Return (PBST)` makes live battery
 *    telemetry (state, power draw, remaining capacity, voltage) work natively.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define ACPICA_DESC_TYPE_OPERAND	0x0e
#define ACPICA_METHOD_AML_START_OFFSET	32
#define ACPICA_METHOD_AML_LEN_OFFSET	48

/*
 * Exact AML byte sequence in \_SB.FFA0.RBUF:
 *   5b 13             CreateFieldOp
 *   42 55 46 46       "BUFF"
 *   0a d0             ByteConst 0xD0 (208 bits)
 *   0b 90 01          WordConst 0x0190 (400 bits)  <-- offset +9: 90 01 -> 30 01 (304 bits)
 *   44 41 54 41       "DATA"
 */
static const u8 rbuf_bug_pattern[] = {
	0x5b, 0x13, 0x42, 0x55, 0x46, 0x46,
	0x0a, 0xd0, 0x0b, 0x90, 0x01,
	0x44, 0x41, 0x54, 0x41
};

/*
 * Replacement AML body for \_SB.BAT0._BST:
 *   55 50 42 53       UPBS ()
 *   a4 50 42 53 54    Return (PBST)
 */
static u8 patched_bst_aml[] = {
	0x55, 0x50, 0x42, 0x53,
	0xa4, 0x50, 0x42, 0x53, 0x54
};

static void *rbuf_method_obj;
static u8 *orig_rbuf_aml;
static u32 orig_rbuf_len;
static u8 *patched_rbuf_aml;

static void *bst_method_obj;
static u8 *orig_bst_aml;
static u32 orig_bst_len;

static struct delayed_work rescan_work;
static int rescan_attempts;

static void *get_acpica_method_obj(const char *path)
{
	acpi_handle handle;
	acpi_status status;
	u8 *obj;

	status = acpi_get_handle(NULL, (acpi_string)path, &handle);
	if (ACPI_FAILURE(status) || !handle)
		return NULL;

	/* struct acpi_namespace_node has union acpi_operand_object *object at offset 0 */
	obj = *(u8 **)handle;
	if (!obj)
		return NULL;

	if (obj[8] != ACPICA_DESC_TYPE_OPERAND || obj[9] != ACPI_TYPE_METHOD) {
		pr_err("%s: unexpected ACPICA object header (%02x, %02x)\n",
		       path, obj[8], obj[9]);
		return NULL;
	}

	return obj;
}

static int patch_rbuf_aml(void)
{
	u8 *aml_start;
	u32 aml_len;
	u32 i;

	rbuf_method_obj = get_acpica_method_obj("\\_SB.FFA0.RBUF");
	if (!rbuf_method_obj) {
		pr_err("\\_SB.FFA0.RBUF not found\n");
		return -ENODEV;
	}

	aml_start = *(u8 **)((u8 *)rbuf_method_obj + ACPICA_METHOD_AML_START_OFFSET);
	aml_len = *(u32 *)((u8 *)rbuf_method_obj + ACPICA_METHOD_AML_LEN_OFFSET);
	if (!aml_start || aml_len < sizeof(rbuf_bug_pattern)) {
		pr_err("\\_SB.FFA0.RBUF invalid AML pointer/length (%u)\n", aml_len);
		return -EINVAL;
	}

	patched_rbuf_aml = kmemdup(aml_start, aml_len, GFP_KERNEL);
	if (!patched_rbuf_aml)
		return -ENOMEM;

	for (i = 0; i <= aml_len - sizeof(rbuf_bug_pattern); i++) {
		if (memcmp(patched_rbuf_aml + i, rbuf_bug_pattern,
			   sizeof(rbuf_bug_pattern)) == 0) {
			/* Clamp bit length from 0x0190 (400) to 0x0130 (304) */
			patched_rbuf_aml[i + 9] = 0x30;
			patched_rbuf_aml[i + 10] = 0x01;

			orig_rbuf_aml = aml_start;
			orig_rbuf_len = aml_len;
			*(u8 **)((u8 *)rbuf_method_obj + ACPICA_METHOD_AML_START_OFFSET) =
				patched_rbuf_aml;

			pr_info("Patched \\_SB.FFA0.RBUF CreateField DATA bit length (0x0190 -> 0x0130 at +0x%x)\n",
				i);
			return 0;
		}
	}

	kfree(patched_rbuf_aml);
	patched_rbuf_aml = NULL;
	pr_err("CreateField pattern not found in \\_SB.FFA0.RBUF\n");
	return -ENOENT;
}

static int patch_bst_aml(void)
{
	bst_method_obj = get_acpica_method_obj("\\_SB.BAT0._BST");
	if (!bst_method_obj) {
		pr_warn("\\_SB.BAT0._BST not found, skipping _BST patch\n");
		return -ENODEV;
	}

	orig_bst_aml = *(u8 **)((u8 *)bst_method_obj + ACPICA_METHOD_AML_START_OFFSET);
	orig_bst_len = *(u32 *)((u8 *)bst_method_obj + ACPICA_METHOD_AML_LEN_OFFSET);

	*(u8 **)((u8 *)bst_method_obj + ACPICA_METHOD_AML_START_OFFSET) = patched_bst_aml;
	*(u32 *)((u8 *)bst_method_obj + ACPICA_METHOD_AML_LEN_OFFSET) = sizeof(patched_bst_aml);

	pr_info("Redirected \\_SB.BAT0._BST to evaluate \\_SB.BAT0.UPBS()\n");
	return 0;
}

static void attach_platform_dev_by_name(const char *name)
{
	struct device *dev;
	int ret;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, name);
	if (!dev)
		return;

	if (!dev->driver) {
		ret = device_attach(dev);
		pr_info("Re-probed platform device %s (ret=%d)\n", name, ret);
	}
	put_device(dev);
}

static void rescan_ffa_consumers(struct work_struct *work)
{
	acpi_handle ffa0_handle, bat0_handle;
	unsigned long long aval = 0;
	struct acpi_device *bat_adev;
	struct device *bat_pdev;
	acpi_status status;
	int ret;

	if (ACPI_FAILURE(acpi_get_handle(NULL, "\\_SB.FFA0", &ffa0_handle)))
		return;

	status = acpi_evaluate_integer(ffa0_handle, "AVAL", NULL, &aval);
	if (ACPI_FAILURE(status) || aval != 1) {
		if (++rescan_attempts < 30)
			schedule_delayed_work(&rescan_work, msecs_to_jiffies(500));
		return;
	}

	/* 1. Update \_SB.BAT0 status and create its platform device if missing */
	if (ACPI_SUCCESS(acpi_get_handle(NULL, "\\_SB.BAT0", &bat0_handle))) {
		bat_adev = acpi_fetch_acpi_dev(bat0_handle);
		if (bat_adev) {
			ret = acpi_bus_get_status(bat_adev);
			pr_info("\\_SB.BAT0 status updated (ret=%d, present=%d, enabled=%d)\n",
				ret, bat_adev->status.present, bat_adev->status.enabled);

			bat_pdev = bus_find_device_by_name(&platform_bus_type, NULL, "PNP0C0A:00");
			if (!bat_pdev) {
				acpi_create_platform_device(bat_adev, NULL);
				pr_info("Created platform device PNP0C0A:00 for \\_SB.BAT0\n");
			} else {
				put_device(bat_pdev);
			}
		}
	}

	/* 2. Attach drivers to all FF-A EC consumer platform devices */
	attach_platform_dev_by_name("PNP0C0A:00");
	attach_platform_dev_by_name("ACPI0003:00");
	attach_platform_dev_by_name("LNXTHERM:00");
	attach_platform_dev_by_name("USBC000:00");
	attach_platform_dev_by_name("PNP0C0B:00");
}

static int __init omnibook_ffa_fix_init(void)
{
	int ret;

	ret = patch_rbuf_aml();
	if (ret)
		return ret;

	patch_bst_aml();

	INIT_DELAYED_WORK(&rescan_work, rescan_ffa_consumers);
	rescan_ffa_consumers(&rescan_work.work);

	return 0;
}

static void __exit omnibook_ffa_fix_exit(void)
{
	cancel_delayed_work_sync(&rescan_work);

	if (bst_method_obj && orig_bst_aml) {
		*(u8 **)((u8 *)bst_method_obj + ACPICA_METHOD_AML_START_OFFSET) = orig_bst_aml;
		*(u32 *)((u8 *)bst_method_obj + ACPICA_METHOD_AML_LEN_OFFSET) = orig_bst_len;
	}

	if (rbuf_method_obj && orig_rbuf_aml) {
		*(u8 **)((u8 *)rbuf_method_obj + ACPICA_METHOD_AML_START_OFFSET) = orig_rbuf_aml;
		*(u32 *)((u8 *)rbuf_method_obj + ACPICA_METHOD_AML_LEN_OFFSET) = orig_rbuf_len;
	}

	kfree(patched_rbuf_aml);
	pr_info("Restored original \\_SB.FFA0.RBUF and \\_SB.BAT0._BST AML\n");
}

module_init(omnibook_ffa_fix_init);
module_exit(omnibook_ffa_fix_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("keejkrej");
MODULE_DESCRIPTION("HP OmniBook Ultra 16-bk0xxx ACPI FF-A EC RBUF & Battery Fix");
