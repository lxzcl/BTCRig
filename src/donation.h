#ifndef BTCRIG_DONATION_H
#define BTCRIG_DONATION_H

#define DONATION_DEFAULT_LEVEL 1
#define DONATION_MINIMUM_LEVEL 1
#define DONATION_CYCLE_MINUTES 100
#define DONATION_USER "bc1qqz0wutk9kk5mmaf7fu4dm5w4fq4fhaah9hpzr3"

int donation_level_valid(int level);
double donation_phase_seconds(int level, int donating);
double donation_initial_user_seconds(int level);

#endif
