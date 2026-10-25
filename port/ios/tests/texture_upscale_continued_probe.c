/* texture_upscale_continued.h: the import phase's continued processing (iOS 26's BGContinuedProcessingTask) */
#include <assert.h>
#include <stdio.h>
#include "texture_upscale_continued.h"

#define STEP(event) texture_upscale_continued_step(&state, TEXTURE_UPSCALE_CONTINUED_##event)

int main(void)
{
	enum texture_upscale_continued_state state = TEXTURE_UPSCALE_CONTINUED_IDLE;

	/* the phase on screen submits once; the launch reports; the phase's end completes successfully */
	assert(STEP(PHASE_BEGAN) == TEXTURE_UPSCALE_CONTINUED_SUBMIT);
	assert(STEP(PHASE_BEGAN) == TEXTURE_UPSCALE_CONTINUED_NOTHING);
	assert(STEP(LAUNCHED) == TEXTURE_UPSCALE_CONTINUED_REPORT && state == TEXTURE_UPSCALE_CONTINUED_RUNNING);
	assert(STEP(PHASE_ENDED) == TEXTURE_UPSCALE_CONTINUED_COMPLETE && state == TEXTURE_UPSCALE_CONTINUED_IDLE);
	/* expiration stops the generator and fails the task, once; the phase's later end does nothing more */
	assert(STEP(PHASE_BEGAN) == TEXTURE_UPSCALE_CONTINUED_SUBMIT);
	assert(STEP(LAUNCHED) == TEXTURE_UPSCALE_CONTINUED_REPORT);
	assert(STEP(EXPIRED) == TEXTURE_UPSCALE_CONTINUED_STOP_AND_FAIL && state == TEXTURE_UPSCALE_CONTINUED_ENDED);
	assert(STEP(EXPIRED) == TEXTURE_UPSCALE_CONTINUED_NOTHING);
	assert(STEP(PHASE_BEGAN) == TEXTURE_UPSCALE_CONTINUED_SUBMIT);   /* a new import phase may ask again */
	assert(STEP(PHASE_ENDED) == TEXTURE_UPSCALE_CONTINUED_NOTHING);
	/* a request that ran only after the phase ended has nothing to do and completes at once */
	assert(STEP(LAUNCHED) == TEXTURE_UPSCALE_CONTINUED_COMPLETE && state == TEXTURE_UPSCALE_CONTINUED_IDLE);
	/* a refused submission ends it; an expiration that was never running does nothing */
	assert(STEP(PHASE_BEGAN) == TEXTURE_UPSCALE_CONTINUED_SUBMIT);
	assert(STEP(REFUSED) == TEXTURE_UPSCALE_CONTINUED_NOTHING && state == TEXTURE_UPSCALE_CONTINUED_ENDED);
	assert(STEP(EXPIRED) == TEXTURE_UPSCALE_CONTINUED_NOTHING);
	assert(STEP(PHASE_ENDED) == TEXTURE_UPSCALE_CONTINUED_NOTHING && state == TEXTURE_UPSCALE_CONTINUED_IDLE);
	puts("texture_upscale_continued_probe: ok");
	return 0;
}
