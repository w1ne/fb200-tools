#ifndef FB200_CONSOLE_H
#define FB200_CONSOLE_H
/* Line-based USB console: typed lines are echoed, dispatched to commands and
 * answered through cdc_log. Non-blocking; safe to call from the main loop. */
void console_init(void);
void console_task(void);
int console_heartbeat_on(void);
void console_reboot(void);   /* defined in main.c */
#endif
