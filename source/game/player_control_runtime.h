/*
PLAYER_CONTROL_RUNTIME.H
*/

#ifndef __PLAYER_CONTROL_RUNTIME_H
#define __PLAYER_CONTROL_RUNTIME_H
#pragma once

/* ---------- headers */

#include "cseries/cseries.h"

/* ---------- prototypes/PLAYER_CONTROL.C */

boolean player_control_update(
	real time_delta_sec);

/* port: apply the newly injected recorded action to its local camera control. */
void player_control_apply_replay(short local_player_index);

#endif // __PLAYER_CONTROL_RUNTIME_H
