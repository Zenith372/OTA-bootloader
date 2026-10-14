#include "led.h"
#include "sys.h"

void led_init(void)
{
	GPIO_InitTypeDef gpio_initstruct;
	__HAL_RCC_GPIOC_CLK_ENABLE();
	gpio_initstruct.Pin = GPIO_PIN_13;
	gpio_initstruct.Mode = GPIO_MODE_OUTPUT_PP;
	gpio_initstruct.Pull = GPIO_PULLUP;
	gpio_initstruct.Speed = GPIO_SPEED_FREQ_HIGH;
	HAL_GPIO_Init(GPIOC, &gpio_initstruct);
	led1_off();
}

void led1_on(void)
{
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
}
void led1_off(void)
{
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
}
