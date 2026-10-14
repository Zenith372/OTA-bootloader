#ifndef __USART_H__
#define __USART_H__

#include "sys.h"
#define UART1_RX_BUF_SIZE 256
#define UART1_TX_BUF_SIZE 256

extern UART_HandleTypeDef uart1_handle;
extern uint8_t uart1_rx_buf[UART1_RX_BUF_SIZE];
#define UART_ERROR 0
#define UART_OK 1
#define UART_TIMEOUT 3
#define UART_INVALID 4

void uart1_init(uint32_t baudrate);
void uart1_receive_test(void);
void uart1_rx_clear(void);
void dma_init(void);
#endif
