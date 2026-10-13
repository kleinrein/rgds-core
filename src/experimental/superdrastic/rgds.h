#pragma once
int rgds_sd_poll(void *event);
void rgds_sd_menu_state(int open);
void rgds_sd_exit(const char *state);
void rgds_sd_settings(int volume, int brightness);
int rgds_sd_value(const char *key, int fallback);
int rgds_sd_battery(int *charging);
int rgds_sd_device_matches(const char *path, const char *name);
/* Small entry points added to the pinned upstream menu/resume units. */
void rgds_sd_menu_toggle(void);
int rgds_sd_idle(void);
int rgds_sd_menu_needs_settle(void);
int rgds_shared_enabled(void);
int rgds_shared_blocked(void);
int rgds_shared_poll(void *event);
void rgds_shared_job_done(int save, int ok);
