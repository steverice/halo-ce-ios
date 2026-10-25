/* The import phase's continued processing (iOS 26's BGContinuedProcessingTask, port/ios/host/host_texture_upscale_continued.m):
the rules as a state machine, header-only so a host probe can check them without the app.

The player starting the import's "Upscaling textures" phase is the explicit user action the task type requires: the
request is submitted then. While the task runs, the upscaler keeps going if the player switches to another app, and
the system shows its progress (textures ready of the whole game's). Expiration stops the generator (the texture in
hand is made again later; finished ones are already in the manifest) and completes the task unsuccessfully; the end
of the import phase (finished or skipped) completes it successfully. Once over, a request is not submitted again
until the next import phase. Gameplay never overlaps it: the import phase is before the game starts. */
#pragma once

enum texture_upscale_continued_state
{
	TEXTURE_UPSCALE_CONTINUED_IDLE,       /* no import phase, or the API is not available here */
	TEXTURE_UPSCALE_CONTINUED_SUBMITTED,  /* the request is with the scheduler */
	TEXTURE_UPSCALE_CONTINUED_RUNNING,    /* the system launched it: progress is reported */
	TEXTURE_UPSCALE_CONTINUED_ENDED       /* completed, expired or refused: nothing more this import phase */
};

enum texture_upscale_continued_event
{
	TEXTURE_UPSCALE_CONTINUED_PHASE_BEGAN,   /* the import's upscaling phase is on screen (a request is submitted) */
	TEXTURE_UPSCALE_CONTINUED_REFUSED,       /* the submission failed */
	TEXTURE_UPSCALE_CONTINUED_LAUNCHED,      /* the scheduler ran the task's handler */
	TEXTURE_UPSCALE_CONTINUED_EXPIRED,       /* its expiration handler */
	TEXTURE_UPSCALE_CONTINUED_PHASE_ENDED    /* the import phase finished or was skipped */
};

/* what the caller must do after a transition */
enum texture_upscale_continued_action
{
	TEXTURE_UPSCALE_CONTINUED_NOTHING,
	TEXTURE_UPSCALE_CONTINUED_SUBMIT,          /* submit the request */
	TEXTURE_UPSCALE_CONTINUED_REPORT,          /* start reporting progress on the task */
	TEXTURE_UPSCALE_CONTINUED_STOP_AND_FAIL,   /* stop the generator, then complete the task unsuccessfully */
	TEXTURE_UPSCALE_CONTINUED_COMPLETE         /* complete the task successfully */
};

static inline enum texture_upscale_continued_action texture_upscale_continued_step(
	enum texture_upscale_continued_state *state, enum texture_upscale_continued_event event)
{
	switch (event)
	{
	case TEXTURE_UPSCALE_CONTINUED_PHASE_BEGAN:
		if (*state == TEXTURE_UPSCALE_CONTINUED_SUBMITTED || *state == TEXTURE_UPSCALE_CONTINUED_RUNNING)
			return TEXTURE_UPSCALE_CONTINUED_NOTHING;
		*state = TEXTURE_UPSCALE_CONTINUED_SUBMITTED;
		return TEXTURE_UPSCALE_CONTINUED_SUBMIT;
	case TEXTURE_UPSCALE_CONTINUED_REFUSED:
		if (*state == TEXTURE_UPSCALE_CONTINUED_SUBMITTED)
			*state = TEXTURE_UPSCALE_CONTINUED_ENDED;
		return TEXTURE_UPSCALE_CONTINUED_NOTHING;
	case TEXTURE_UPSCALE_CONTINUED_LAUNCHED:
		/* (a queued request the scheduler runs after the phase ended has nothing to do: complete it) */
		if (*state != TEXTURE_UPSCALE_CONTINUED_SUBMITTED)
			return TEXTURE_UPSCALE_CONTINUED_COMPLETE;
		*state = TEXTURE_UPSCALE_CONTINUED_RUNNING;
		return TEXTURE_UPSCALE_CONTINUED_REPORT;
	case TEXTURE_UPSCALE_CONTINUED_EXPIRED:
		if (*state != TEXTURE_UPSCALE_CONTINUED_RUNNING)
			return TEXTURE_UPSCALE_CONTINUED_NOTHING;
		*state = TEXTURE_UPSCALE_CONTINUED_ENDED;
		return TEXTURE_UPSCALE_CONTINUED_STOP_AND_FAIL;
	case TEXTURE_UPSCALE_CONTINUED_PHASE_ENDED:
	{
		int running = *state == TEXTURE_UPSCALE_CONTINUED_RUNNING;

		*state = TEXTURE_UPSCALE_CONTINUED_IDLE;
		return running ? TEXTURE_UPSCALE_CONTINUED_COMPLETE : TEXTURE_UPSCALE_CONTINUED_NOTHING;
	}
	}
	return TEXTURE_UPSCALE_CONTINUED_NOTHING;
}
