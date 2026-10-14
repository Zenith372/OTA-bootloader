#include "uart1.h"
#include "stdio.h"
#include "string.h"


uint16_t rx_len = 0; //接收到的数据长度
uint16_t uart1_cnt = 0, uart1_cntPre = 0; //计数器，计数器旧的值

UART_HandleTypeDef uart1_handle = {0};
DMA_HandleTypeDef dma_handle = {0};

//串口1初始化函数
void uart1_init(uint32_t baudrate)
{
	uart1_handle.Instance = USART1; //串口1
	uart1_handle.Init.BaudRate = baudrate; //波特率，从外部传入
	uart1_handle.Init.WordLength = UART_WORDLENGTH_8B; //字长我们用的是 8 位的
	uart1_handle.Init.StopBits = UART_STOPBITS_1; //停止位，一个停止位
	uart1_handle.Init.Parity = UART_PARITY_NONE; //校验位，不使用校验位
	uart1_handle.Init.Mode = UART_MODE_TX_RX; //模式
	uart1_handle.Init.HwFlowCtl = UART_HWCONTROL_NONE;//硬件流控制
	//初始化函数
	HAL_StatusTypeDef ret = HAL_UART_Init(&uart1_handle);
    if(ret != HAL_OK)
    {
        while(1); //卡死在这里代表UART初始化失败
    }
	//配置中断优先级
	HAL_NVIC_SetPriority(USART1_IRQn, 2, 2); //参数1：中断线，参数2和3分别是抢占优先级和响应优先级 
	HAL_NVIC_EnableIRQ(USART1_IRQn); //使能中断，使用 USART1_IRQn 中断线
	//__HAL_UART_ENABLE_IT(huart, UART_IT_RXNE); //使能 UART1 接收中断
	//__HAL_UART_ENABLE_IT(huart, UART_IT_IDLE); //使能 UART1 空闲中断
	//__HAL_UART_ENABLE_IT(&huart, UART_IT_ERR);//USART 错误中断（PE/FE/NE/ORE）：处理串口帧错误、溢出错误等，避免传输异常后卡死。
}

//msp 函数
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
	//三个串口都会去调用msp函数，所以要先判断一下
	if(huart->Instance == USART1)
	{
		//开启时钟
		__HAL_RCC_USART1_CLK_ENABLE(); //使能串口1时钟
		__HAL_RCC_GPIOA_CLK_ENABLE(); //使能 GPIOA 时钟
		
		GPIO_InitTypeDef gpio_initstruct;
		
		//调用 GPIO 初始化函数
		gpio_initstruct.Pin = GPIO_PIN_9; //TX 对应的引脚
		gpio_initstruct.Mode = GPIO_MODE_AF_PP; //复用推挽输出
		gpio_initstruct.Pull = GPIO_PULLUP; //上拉
		gpio_initstruct.Speed = GPIO_SPEED_FREQ_HIGH; //高速
		HAL_GPIO_Init(GPIOA, &gpio_initstruct);
		
		gpio_initstruct.Pin = GPIO_PIN_10; //RX 对应的引脚
		gpio_initstruct.Mode = GPIO_MODE_AF_INPUT; //复用输入模式
		//后面两个参数上个引脚已经初始化了
		HAL_GPIO_Init(GPIOA, &gpio_initstruct);
		
	}
}

/**
* 重定义 fputc 函数
*	printf 函数最终会通过调用 fputc 输出字符串到串口
*
*/
int fputc(int ch, FILE *f)
{
	while((USART1->SR &0x40) == 0); //等待上一次字符发送完成
	
	USART1->DR = (uint8_t)ch; //将要发送的字符 ch 写入到 DR 寄存器
	return ch;
		
}

//初始化 DMA
void dma_init(void)
{
	//打开 DMA 时钟
	__HAL_RCC_DMA1_CLK_ENABLE();
	
	dma_handle.Instance = DMA1_Channel5; //DMA 实例，串口向内存发送，DMA1 通道5
	dma_handle.Init.Direction = DMA_PERIPH_TO_MEMORY; //方向，外设到内存
	
	//内存相关配置(源)
	dma_handle.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE; //内存数据对齐方式，一般用 8 字节对齐
	dma_handle.Init.MemInc = DMA_MINC_ENABLE; //内存数据增长方式
	
	//外设相关配置(目标)
	dma_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE; //外设数据对齐方式
	dma_handle.Init.PeriphInc = DMA_PINC_DISABLE; //外设数据增长方式，不增长，因为每次都是发到串口1的 TDR 寄存器里面
	
	dma_handle.Init.Priority = DMA_PRIORITY_MEDIUM; //优先级，中等
	dma_handle.Init.Mode = DMA_NORMAL; //模式：循环，非循环，内存到内存只支持正常模式，不支持循环模式
	//初始化DMA
	HAL_DMA_Init(&dma_handle);

	HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 1, 0);
	HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);
	//内存到外设的链接
	__HAL_LINKDMA(&uart1_handle, hdmarx, dma_handle);
	__HAL_DMA_ENABLE_IT(&dma_handle, DMA_IT_TC);
}


