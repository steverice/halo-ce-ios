/* The hand's aim assist strength (aim_assist_strength.h). */
#include <math.h>

#include "aim_assist_strength.h"

float aim_assist_strength_clamp(float setting)
{
	if (setting != setting)
		return AIM_ASSIST_STRENGTH_DEFAULT;
	return fminf(fmaxf(setting, 0.0f), AIM_ASSIST_STRENGTH_MAXIMUM);
}

float aim_assist_strength_effective(float setting, int hand_aims, int fixed_rules)
{
	if (!hand_aims || fixed_rules)
		return AIM_ASSIST_STRENGTH_DEFAULT;
	return aim_assist_strength_clamp(setting);
}

void aim_assist_strength_apply(float strength, float *autoaim_angle, float *deviation_angle)
{
	float scaled;

	/* (the game's own values pass through untouched, bit for bit) */
	if (strength == AIM_ASSIST_STRENGTH_DEFAULT || !(*autoaim_angle > 0.0f))
		return;
	scaled = fmaxf(*autoaim_angle * aim_assist_strength_clamp(strength), AIM_ASSIST_ON_TARGET_ANGLE);
	*autoaim_angle = scaled;
	if (scaled > *deviation_angle)
		*deviation_angle = scaled;
}

float aim_assist_strength_step(float setting, int direction)
{
	float steps = roundf(aim_assist_strength_clamp(setting) / AIM_ASSIST_STRENGTH_STEP) + (direction > 0 ? 1.0f : -1.0f);

	return aim_assist_strength_clamp(steps * AIM_ASSIST_STRENGTH_STEP);
}
