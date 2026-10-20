/* The hand's aim assist strength (input.aim_assist): how much of the game's
bullet autoaim a hand-aimed shot keeps. Pure math with no game headers, so a
host-side test can check it (port/ios/tests/aim_assist_probe.c);
controller_aim_game.c reads the setting and the game's state, and
source/game/aim_assist.c applies the result.

The strength scales the weapon's autoaim angle, the cone around the aim
inside which a shot is bent onto a target: 0 leaves the shot where the hand
points, 1 is the game's own cone, 2 twice it. The autoaim range, the
magnetism cone that finds a target (the needler's homing) and the
deviation cone that converges the shot on the aim are the game's at every
strength. */
#ifndef AIM_ASSIST_STRENGTH_H
#define AIM_ASSIST_STRENGTH_H

#define AIM_ASSIST_STRENGTH_DEFAULT 1.0f   /* the game's own values */
#define AIM_ASSIST_STRENGTH_MAXIMUM 2.0f
#define AIM_ASSIST_STRENGTH_STEP 0.5f      /* the live chord's step */
/* the least autoaim angle a weapon that has one keeps (radians, 0.1
degrees): at strength 0 a target the aim's line passes through still counts
as aimed at (the reticle turns red on it), and a shot is bent by less than
half of this */
#define AIM_ASSIST_ON_TARGET_ANGLE 0.0017453292f

/* the setting into 0 to AIM_ASSIST_STRENGTH_MAXIMUM; the default for a
value that isn't a number */
float aim_assist_strength_clamp(float setting);

/* the strength a shot or a targeting query takes: the setting only while a
hand aims it in a game whose rules this machine owns. fixed_rules (a network
game, or a multiplayer game type) and a stick's, a mouse's or a head's aim
get AIM_ASSIST_STRENGTH_DEFAULT whatever the setting says */
float aim_assist_strength_effective(float setting, int hand_aims, int fixed_rules);

/* a weapon's autoaim and deviation angles (radians, already over the zoom's
magnification) at a strength. A weapon without autoaim keeps none. The
deviation cone, which the game makes no narrower than the autoaim cone,
widens with it above strength 1 and is the game's below */
void aim_assist_strength_apply(float strength, float *autoaim_angle, float *deviation_angle);

/* the setting a step up (direction 1) or down (-1) from here, on multiples
of AIM_ASSIST_STRENGTH_STEP, held at the ends */
float aim_assist_strength_step(float setting, int direction);

#endif
