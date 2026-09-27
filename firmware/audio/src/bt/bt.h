#ifndef FB200_BT_H
#define FB200_BT_H
#include <stddef.h>
#include <stdint.h>
/* Bluetooth module link: LPUART5 (GPIO_B1_12 TX / B1_13 RX), 115200 8N1,
 * like the stock (docs/UI_AND_STORAGE.md §4). The module takes AT commands
 * and is otherwise a transparent BLE UART carrying the app's AA 55 frames. */
void bt_init(void);
void bt_task(uint32_t now_ms);
int bt_send(const uint8_t *data, size_t n);     /* non-blocking; -1 while busy/full */
int bt_at(const char *cmd);                     /* sends cmd + "\r\n" */
void bt_status(void);                           /* console dump */
/* Received bytes that are not AT replies go here (app protocol). Weak: the
 * protocol layer overrides it. */
void bt_rx_frame_bytes(const uint8_t *data, size_t n);
#endif
