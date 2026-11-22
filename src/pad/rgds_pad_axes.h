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

/* The ABS codes adc-joystick reports for mux states 0..3 (the RG DS tree maps state N to these, raw), and the pad
 * axis each state becomes, as ROCKNIX's driver reported it. That driver drives the mux select lines inverted against
 * mainline's gpio-mux: its channel c sets (sel_a, sel_b) = (1,1), (1,0), (0,1), (0,0) for c = 0..3, which is mux
 * state 3, 1, 2, 0. With its amux-channel-mapping <1 0 2 3> and report types ABS_RY, ABS_RX, ABS_Y, ABS_X (and
 * invert-absx, -absy, -absry) that makes: state 0 -> X inverted, 1 -> RY inverted, 2 -> Y inverted, 3 -> RX. Its
 * sticks for the radial deadzone pair its adcs 0+1 (states 1 and 3, the right stick) and 2+3 (states 2 and 0, the
 * left). Mainline's own channel names agree (state 0 left_x, 1 right_x, 2 left_y, 3 right_y). tests/test_rgds_pad.py
 * derives this table from the driver's switch again. */
#define RGDS_PAD_ABS_X 0x00
#define RGDS_PAD_ABS_Y 0x01
#define RGDS_PAD_ABS_RX 0x03
#define RGDS_PAD_ABS_RY 0x04
static const int rgds_pad_state_code[4] = {RGDS_PAD_ABS_X, RGDS_PAD_ABS_Y, RGDS_PAD_ABS_RX, RGDS_PAD_ABS_RY};
static const int rgds_pad_state_axis[4] = {RGDS_PAD_ABS_X, RGDS_PAD_ABS_RY, RGDS_PAD_ABS_Y, RGDS_PAD_ABS_RX};
static const int rgds_pad_state_invert[4] = {1, 1, 1, 0};
/* each stick's two mux states: right (RY, RX), left (Y, X) */
static const int rgds_pad_stick_states[2][2] = {{1, 3}, {2, 0}};

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
