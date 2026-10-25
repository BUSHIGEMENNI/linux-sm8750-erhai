/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Osilvfe <osilvfe@gmail.com> */
/*
 * SC820CS 3264x2448 baseline power-up sequence from the OnePlus Pad Pro
 * ("caihong", SM8650) downstream camera tree.
 *
 * Taken verbatim from the out-of-tree driver at
 * https://github.com/Osilvfe/out_tree_module (camera/sc820cs-caihong-regs.h,
 * GPL-2.0-only), where it is hardware-verified to stream 3264x2448 RAW10 at
 * 24 fps on 4 MIPI data lanes (link frequency 366 MHz, pixel rate
 * 233.28 MHz, HTS 3888 / VTS 2500).
 *
 * Cross-validation against the OnePlus Pad 2 Pro ("erhai", SM8750) odm blob
 * com.qti.sensormodule.erhaifront.bin (com.qti.sensormodule blob, Parameter
 * Parser V5.5.1): the blob's flat register-write script (56-byte records,
 * little-endian u16 register at offset 0, script base 0x7b90) contains this
 * exact register sequence as a strict subsequence - every register below
 * appears, in the same order.  The erhai script adds 9 analog-tuning
 * registers on top (0x3307, 0x330f, 0x3310, 0x3317, 0x3347, 0x3650,
 * 0x3651, 0x3c04 and a trailing 0x3221 flip/mirror write); their values are
 * not decoded yet and are the residual tuning delta between the two boards.
 *
 * 0x301f = 0x0e selects 4-lane MIPI output.  Stream control (0x0100) is
 * intentionally handled by the V4L2 driver after this list has been
 * written.  The list starts with a soft reset (0x0103 = 1); the driver
 * inserts a settle delay after that first entry - see
 * sc820cs_enable_streams() in sc820cs.c.
 */
#ifndef SC820CS_CAIHONG_REGS_H
#define SC820CS_CAIHONG_REGS_H

static const struct cci_reg_sequence sc820cs_caihong_3264x2448_regs[] = {
	{ CCI_REG8(0x0103), 0x01 },
	{ CCI_REG8(0x36e9), 0x80 },
	{ CCI_REG8(0x37f9), 0x80 },
	{ CCI_REG8(0x36e9), 0x24 },
	{ CCI_REG8(0x37f9), 0x24 },
	{ CCI_REG8(0x301f), 0x0e },
	{ CCI_REG8(0x3205), 0xc7 },
	{ CCI_REG8(0x3211), 0x04 },
	{ CCI_REG8(0x3270), 0x00 },
	{ CCI_REG8(0x3271), 0x00 },
	{ CCI_REG8(0x3272), 0x00 },
	{ CCI_REG8(0x3273), 0x03 },
	{ CCI_REG8(0x3301), 0x08 },
	{ CCI_REG8(0x3303), 0x10 },
	{ CCI_REG8(0x3306), 0x84 },
	{ CCI_REG8(0x3309), 0x88 },
	{ CCI_REG8(0x330a), 0x01 },
	{ CCI_REG8(0x330b), 0x0c },
	{ CCI_REG8(0x330c), 0x10 },
	{ CCI_REG8(0x330d), 0x18 },
	{ CCI_REG8(0x330e), 0x30 },
	{ CCI_REG8(0x3314), 0x15 },
	{ CCI_REG8(0x331f), 0x79 },
	{ CCI_REG8(0x3326), 0x0e },
	{ CCI_REG8(0x3327), 0x0a },
	{ CCI_REG8(0x3329), 0x0b },
	{ CCI_REG8(0x3333), 0x10 },
	{ CCI_REG8(0x3334), 0x40 },
	{ CCI_REG8(0x335d), 0x60 },
	{ CCI_REG8(0x3364), 0x56 },
	{ CCI_REG8(0x336c), 0xce },
	{ CCI_REG8(0x3390), 0x08 },
	{ CCI_REG8(0x3391), 0x09 },
	{ CCI_REG8(0x3392), 0x0f },
	{ CCI_REG8(0x3393), 0x10 },
	{ CCI_REG8(0x3394), 0x20 },
	{ CCI_REG8(0x3395), 0x28 },
	{ CCI_REG8(0x33ad), 0x3c },
	{ CCI_REG8(0x33af), 0x70 },
	{ CCI_REG8(0x33b2), 0x70 },
	{ CCI_REG8(0x33b3), 0x40 },
	{ CCI_REG8(0x349f), 0x1e },
	{ CCI_REG8(0x34a6), 0x09 },
	{ CCI_REG8(0x34a7), 0x0f },
	{ CCI_REG8(0x34a8), 0x30 },
	{ CCI_REG8(0x34a9), 0x20 },
	{ CCI_REG8(0x34f8), 0x1f },
	{ CCI_REG8(0x34f9), 0x08 },
	{ CCI_REG8(0x3637), 0x43 },
	{ CCI_REG8(0x363c), 0x8d },
	{ CCI_REG8(0x3670), 0x4a },
	{ CCI_REG8(0x3674), 0xf6 },
	{ CCI_REG8(0x3675), 0xdc },
	{ CCI_REG8(0x3676), 0xcc },
	{ CCI_REG8(0x367c), 0x09 },
	{ CCI_REG8(0x367d), 0x0f },
	{ CCI_REG8(0x3690), 0x34 },
	{ CCI_REG8(0x3691), 0x44 },
	{ CCI_REG8(0x3692), 0x55 },
	{ CCI_REG8(0x3698), 0x86 },
	{ CCI_REG8(0x3699), 0x8d },
	{ CCI_REG8(0x369a), 0x99 },
	{ CCI_REG8(0x369b), 0xb7 },
	{ CCI_REG8(0x369c), 0x0f },
	{ CCI_REG8(0x369d), 0x1f },
	{ CCI_REG8(0x36a2), 0x09 },
	{ CCI_REG8(0x36a3), 0x0b },
	{ CCI_REG8(0x36a4), 0x0f },
	{ CCI_REG8(0x36b0), 0x48 },
	{ CCI_REG8(0x36b1), 0x38 },
	{ CCI_REG8(0x36b2), 0x41 },
	{ CCI_REG8(0x370f), 0x01 },
	{ CCI_REG8(0x3724), 0xc1 },
	{ CCI_REG8(0x3771), 0x07 },
	{ CCI_REG8(0x3772), 0x03 },
	{ CCI_REG8(0x3773), 0x63 },
	{ CCI_REG8(0x377a), 0x08 },
	{ CCI_REG8(0x377b), 0x0f },
	{ CCI_REG8(0x3901), 0x04 },
	{ CCI_REG8(0x3903), 0xa0 },
	{ CCI_REG8(0x3905), 0x8d },
	{ CCI_REG8(0x391d), 0x01 },
	{ CCI_REG8(0x3926), 0x23 },
	{ CCI_REG8(0x393f), 0x80 },
	{ CCI_REG8(0x3940), 0x00 },
	{ CCI_REG8(0x3941), 0x00 },
	{ CCI_REG8(0x3942), 0x00 },
	{ CCI_REG8(0x3943), 0x63 },
	{ CCI_REG8(0x3944), 0x5f },
	{ CCI_REG8(0x3e00), 0x01 },
	{ CCI_REG8(0x3e01), 0x38 },
	{ CCI_REG8(0x3e02), 0x00 },
	{ CCI_REG8(0x4401), 0x13 },
	{ CCI_REG8(0x4402), 0x03 },
	{ CCI_REG8(0x4403), 0x0e },
	{ CCI_REG8(0x4404), 0x28 },
	{ CCI_REG8(0x4405), 0x34 },
	{ CCI_REG8(0x4407), 0x0e },
	{ CCI_REG8(0x440c), 0x42 },
	{ CCI_REG8(0x440d), 0x42 },
	{ CCI_REG8(0x440e), 0x32 },
	{ CCI_REG8(0x440f), 0x53 },
	{ CCI_REG8(0x4412), 0x01 },
	{ CCI_REG8(0x4424), 0x01 },
	{ CCI_REG8(0x442d), 0x00 },
	{ CCI_REG8(0x442e), 0x00 },
	{ CCI_REG8(0x4509), 0x28 },
	{ CCI_REG8(0x450d), 0x18 },
	{ CCI_REG8(0x451d), 0xc8 },
	{ CCI_REG8(0x4526), 0x09 },
	{ CCI_REG8(0x5000), 0x0e },
	{ CCI_REG8(0x550e), 0x00 },
	{ CCI_REG8(0x550f), 0xBC },
	{ CCI_REG8(0x5780), 0x66 },
	{ CCI_REG8(0x578d), 0x40 },
	{ CCI_REG8(0x57aa), 0xeb },
};

#endif
