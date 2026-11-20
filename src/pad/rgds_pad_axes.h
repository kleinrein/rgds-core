/* The stick arithmetic of RG DS OS's pad (rgds-pad.c), kept free of Linux headers so tests/test_rgds_pad.py can run
 * it on any computer.
 *
 * It is the arithmetic of ROCKNIX's rocknix-singleadc-joypad driver, which the RG DS ran until the mainline input
 * drivers (docs/OWN_BSP_PLAN.md, Phase C step 2), so the sticks feel the same: each raw ADC reading times the scale,
 * minus the same at the moment the pad was opened (the rest position); a scaled radial deadzone per stick; a gain
 * ("tuning", in percent) per direction; clamping to +-max; and an inversion per axis. Integer maths throughout, in
 * the same order as the driver, so the values match it to the unit.
 */
#ifndef RGDS_PAD_AXES_H
#define RGDS_PAD_AXES_H

/* The RG DS's settings, from its device tree for ROCKNIX's driver (button-adc-scale, -deadzone, abs_*-tuning). */
#define RGDS_PAD_SCALE 2
#define RGDS_PAD_MAX (1800 / 2 * RGDS_PAD_SCALE) /* ADC_MAX_VOLTAGE / 2, times the scale */
#define RGDS_PAD_DEADZONE 64
#define RGDS_PAD_TUNING 250
#define RGDS_PAD_FUZZ 32
#define RGDS_PAD_FLAT 32

struct rgds_pad_axis {
	int code;   /* the ABS_* code reported */
	int invert; /* report the value negated */
	int cal;    /* raw * scale at the rest position */
	int value;  /* the last value worked out, before inversion */
};

/* The kernel's int_sqrt: the floor of the square root. */
static inline long rgds_pad_isqrt(long x)
{
	long r = 0, bit = 1L << 30;
	if (x <= 0)
		return 0;
	while (bit > x)
		bit >>= 2;
	while (bit) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

static inline int rgds_pad_tune(int value)
{
	/* ADC_DATA_TUNING(x, p) = (x * p) / 100, the same percentage both ways on the RG DS */
	if (value != 0)
		value = (value * RGDS_PAD_TUNING) / 100;
	if (value > RGDS_PAD_MAX)
		value = RGDS_PAD_MAX;
	if (value < -RGDS_PAD_MAX)
		value = -RGDS_PAD_MAX;
	return value;
}

/* One stick: two raw readings in, both axes' values worked out (x->value, y->value). Returns 0 when either reading
 * is 0, which the driver takes as a failed read and skips (the values then stay as they were). */
static inline int rgds_pad_stick(struct rgds_pad_axis *x, int raw_x, struct rgds_pad_axis *y, int raw_y)
{
	int vx, vy, mag, max = RGDS_PAD_MAX, dz = RGDS_PAD_DEADZONE;
	if (raw_x == 0 || raw_y == 0)
		return 0;
	vx = raw_x * RGDS_PAD_SCALE - x->cal;
	vy = raw_y * RGDS_PAD_SCALE - y->cal;
	mag = (int)rgds_pad_isqrt((long)vx * vx + (long)vy * vy);
	if (dz) {
		if (mag <= dz) {
			vx = 0;
			vy = 0;
		} else {
			/* the driver's order of operations, which keeps the products inside an int */
			vx = (((max * vx) / mag) * (mag - dz)) / (max - dz);
			vy = (((max * vy) / mag) * (mag - dz)) / (max - dz);
		}
	}
	x->value = rgds_pad_tune(vx);
	y->value = rgds_pad_tune(vy);
	return 1;
}

static inline int rgds_pad_report(const struct rgds_pad_axis *a)
{
	return a->invert ? -a->value : a->value;
}

#endif
