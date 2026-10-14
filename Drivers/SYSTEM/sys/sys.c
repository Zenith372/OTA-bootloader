#include "sys.h"

void stm32_clock_init(uint32_t plln)
{
		HAL_StatusTypeDef ret = HAL_ERROR;//返回值，一般往坏的初始化
    RCC_OscInitTypeDef rcc_osc_init = {0};//初始化结构体
		RCC_ClkInitTypeDef rcc_clk_init = {0};
		
		rcc_osc_init.OscillatorType = RCC_OSCILLATORTYPE_HSE;  //选择要配置HSE
		rcc_osc_init.HSEState = RCC_HSE_ON;  //打开HSE
		rcc_osc_init.HSEPredivValue = RCC_HSE_PREDIV_DIV1;  //HSE预分频系数
		rcc_osc_init.PLL.PLLState = RCC_PLL_ON; //打开PLL
		rcc_osc_init.PLL.PLLSource = RCC_PLLSOURCE_HSE; //PLL时钟源选择HSE 
		rcc_osc_init.PLL.PLLMUL = plln;  //PLL倍频系数
		
		ret = HAL_RCC_OscConfig(&rcc_osc_init); //初始化
		
		if (ret != HAL_OK)
    {
        while (1);   //如果时钟初始化失败，之后的程序将可能无法正常执行，我们在这里死等
    }
		
		//这几种 Clock 我们都要用到
		rcc_clk_init.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
		rcc_clk_init.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK; //设置系统时钟来自PLL
		rcc_clk_init.AHBCLKDivider = RCC_SYSCLK_DIV1;     //AHB分频系数为 1
		rcc_clk_init.APB1CLKDivider = RCC_HCLK_DIV2;      //APB1分频系数为 2
		rcc_clk_init.APB2CLKDivider = RCC_HCLK_DIV1;     //APB2分频系数为 1
		
		ret = HAL_RCC_ClockConfig(&rcc_clk_init, FLASH_LATENCY_2);  //FLASH_LATENCY_2：因为Flash慢，CPU要取等它，所以隔两个周期取个数
		
		if (ret != HAL_OK)
    {
        while (1);   //如果时钟初始化失败，之后的程序将可能无法正常执行，我们在这里死等
    }
		
}

