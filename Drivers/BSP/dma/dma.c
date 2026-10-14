#include "dma.h"
#include "stdio.h"

DMA_HandleTypeDef dma_handle = {0};

void dma_init(void)
{
	//打开 DMA 时钟
	__HAL_RCC_DMA1_CLK_ENABLE();
	
	dma_handle.Instance = DMA1_Channel5; //DMA 实例，内存到内存，任何一个通道都可以
	dma_handle.Init.Direction = DMA_PERIPH_TO_MEMORY; //方向，内存到内存，内存到外设还是外设到内存等等
	
	//内存相关配置(源)
	dma_handle.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE; //内存数据对齐方式，一般用 8 字节对齐
	dma_handle.Init.MemInc = DMA_MINC_ENABLE; //内存数据增长方式
	
	//外设相关配置(目标)
	dma_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE; //外设数据对齐方式
	dma_handle.Init.PeriphInc = DMA_PINC_DISABLE; //外设数据增长方式
	
	dma_handle.Init.Priority = DMA_PRIORITY_MEDIUM; //优先级，只有一个外设，优先级随便，中等
	dma_handle.Init.Mode = DMA_NORMAL; //模式：循环，非循环，内存到内存只支持正常模式，不支持循环模式
	//初始化DMA
	HAL_DMA_Init(&dma_handle);
	
	// 把DMA句柄绑定到串口RX
  __HAL_LINKDMA(&uart1_handle, hdmarx, hdma_usart1_rx);

  // DMA中断优先级（低于串口中断）
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
}
