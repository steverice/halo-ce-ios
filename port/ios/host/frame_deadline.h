/* display.frame_deadline's deadline for a frame's work (gpu_metal.m), in plain C so that
   port/ios/tests/frame_deadline_probe.c checks it natively. */
#ifndef HALO_FRAME_DEADLINE_H
#define HALO_FRAME_DEADLINE_H

/* the seconds a frame's work has: the period of the frame rate in force (target: display.frame_rate's cap or a scene
   class's rate, host_frame_rate_target; 0 for none), else the display's refresh period; 1/60 s when neither is
   plausible (20 to 240 frames a second) */
static inline double frame_deadline_period(double target, double refresh_period)
{
	double period = target >= 20.0 && target <= 240.0 ? 1.0 / target : refresh_period;

	return period >= 1.0 / 240.0 && period <= 1.0 / 20.0 ? period : 1.0 / 60.0;
}

#endif
