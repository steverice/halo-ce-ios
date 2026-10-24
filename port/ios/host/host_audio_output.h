#pragma once
/* Called from the main-thread event poll; the hint comes from the guest's
   menu update and is false at the main menu and during gameplay. */
void host_audio_output_poll(void);
int host_audio_output_up(void);
