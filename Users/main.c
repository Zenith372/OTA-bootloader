#include "sys.h"
#include "uart1.h"
#include "delay.h"
#include "bootloader.h"
#include "stdio.h"
#include "led.h"
#include "string.h"

#define A "A"
#define B "B"
#define ACTIVE_SLOT temp_flag.active_slot == 1 ? A : B

volatile uint32_t receive_all_len;  //新写入固件总长度
extern uint8_t rx_buf0[UART1_RX_BUF_SIZE];//DMA缓冲区0
extern uint8_t rx_buf1[UART1_RX_BUF_SIZE];//DMA缓冲区1
extern volatile uint16_t buf0_len;//DMA缓冲区0接收长度
extern volatile uint16_t buf1_len;//DMA缓冲区1接收长度
extern volatile uint8_t buf0_ready;//DMA缓冲区0接收完成
extern volatile uint8_t buf1_ready;//DMA缓冲区1接收完成
extern UART_HandleTypeDef uart1_handle;
extern uint8_t cmd_buf[];//命令帧缓冲区
extern uint8_t cmd_idx;//命令帧元素位
extern uint8_t ch_recv;//单字节接收命令帧的缓冲区
extern volatile OTA_FlagTypeDef temp_flag;//标志位
extern uint32_t flash_write_offset;//偏移量
extern uint8_t last_byte_flag;//遗留标志位
extern volatile uint8_t a;//专用调试标志位
extern uint8_t active_buf;//哪一个缓冲区正在接收

int main(void)
{
    HAL_Init();                         /* 初始化HAL库 */
    stm32_clock_init(RCC_PLL_MUL9); /* 设置时钟, 72Mhz */
    //【关键】上电第一时间开启看门狗，全程保护 */
    IWDG_Boot_Init();//开启看门狗
    uart1_init(115200);//初始化串口
    dma_init();//初始化DMA
    Flash_Read(FLAG_START_ADDR, &temp_flag, sizeof(OTA_FlagTypeDef));
    /* 1. 标志区合法性校验：非法则恢复默认值 */
    if(temp_flag.state != OTA_IDLE &&
       temp_flag.state != OTA_DOWNLOADING &&
       temp_flag.state != OTA_VERIFY_OK &&
       temp_flag.state != OTA_RUN_NEW_FAIL)
    {
        temp_flag.state = OTA_IDLE;
        temp_flag.active_slot = SLOT_A;
        temp_flag.boot_cnt = 0;
        OTA_Flag_Save();
        IWDG_Boot_Feed(); // 擦写后喂狗
    }
    // 校验状态：只允许 OTA_IDLE / OTA_VERIFY_OK；其余异常状态全部重置
    if(temp_flag.state != OTA_IDLE && temp_flag.state != OTA_VERIFY_OK)
    {
        temp_flag.state = OTA_IDLE;
        temp_flag.boot_cnt = 0;
        OTA_Flag_Save(); // 写回Flash，清除异常DOWNLOADING标记
    }
    printf("当前活跃APP区为:%s\r\n",ACTIVE_SLOT);
    printf("等待接收命令，判断当前状态中...\r\n");
    HAL_UART_Receive_IT(&uart1_handle, &ch_recv, 1);// 启动单字节中断接收【接收指令】
    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
    uint32_t start_tick = HAL_GetTick();
    while((HAL_GetTick() - start_tick) < 5000)
    {
        //printf("%d\r\n",a);
        IWDG_Boot_Feed(); //循环喂狗，防止看门狗复位
        // 如果已经收到完整升级命令，提前跳出等待窗口
        if(temp_flag.state == OTA_DOWNLOADING)
        {
            printf("111");
            break;
        }
    }
    if(temp_flag.state == OTA_VERIFY_OK)//固件下载好后的判断状态
    {
        temp_flag.boot_cnt++;
        OTA_Flag_Save();
        if(temp_flag.boot_cnt >= 3)
        {
            //连续2次启动失败，标记失败，回滚到A区（旧固件）
            temp_flag.state = OTA_RUN_NEW_FAIL;
            temp_flag.boot_cnt = 0;
            OTA_Flag_Save();
            bootloader_jump_to_app(GetSlotStartAddr(temp_flag.active_slot),GetSlotEndAddr(temp_flag.active_slot));
        }
        else
        {
            printf("直接跳转新APP...\r\n");
            bootloader_jump_to_app(GetSlotUpdateAddr(temp_flag.active_slot),GetSlotUpdateEndAddr(temp_flag.active_slot));//跳转APP
        }
    }
    else if(temp_flag.state == OTA_DOWNLOADING)//升级下载状态
    {
        //==== 1. 收到OTA开始指令，进入固件接收状态 ====
        OTA_Flag_Save();
        printf("命令帧格式正确,进入下载固件状态,等待下载固件包进行升级\r\n");
        HAL_UART_AbortReceive_IT(&uart1_handle);//关 RXNE 中断 + 修正 HAL 状态。
        //初始化偏移量，遗留字节，接收总长度
        flash_write_offset = 0;
        last_byte_flag = 0;
        receive_all_len = 0;
        if(!Flash_erase())//flash的非当前活跃APP区全擦
        {
            printf("擦除失败,中止本次升级\r\n");
            temp_flag.state = OTA_IDLE;
            OTA_Flag_Save();
            return 1;      // 回到等待命令,不启动 DMA
        }
        printf("擦除成功\r\n");
        // 启动DMA乒乓接收固件
        HAL_StatusTypeDef dma_ret = HAL_UART_Receive_DMA(&uart1_handle, rx_buf0, UART1_RX_BUF_SIZE);
        // 打开IDLE中断前，先清除一次残留的IDLE标志
        __HAL_UART_CLEAR_IDLEFLAG(&uart1_handle);
        __HAL_UART_ENABLE_IT(&uart1_handle, UART_IT_IDLE);//打开空闲中断
        printf("启动DMA接收, ret=%d\r\n", dma_ret);  // 新增：0=启动成功
    }
    else//空闲状态OTA_IDLE || OTA_RUN_FAIL
    {
        if(temp_flag.active_slot == SLOT_A)
        {
            bootloader_jump_to_app(APP_A_START_ADDR,APP_A_END_ADDR);
        }
        else
        {
            bootloader_jump_to_app(APP_B_START_ADDR,APP_B_END_ADDR);
        }
        
    }
    while(1)
    {
        // ========== 检测buf0就绪 ==========
        if(buf0_ready == 1)
        {
            // 临界区：防止读取变量中途被DMA中断改写
            __disable_irq();////关闭中断
            uint16_t len = buf0_len;
            __enable_irq();////使能中断
            Flash_write(rx_buf0, len);// 处理缓冲区：把rx_buf0[0 ~ len-1]写入Flash
            receive_all_len += len;//每次写入，记录写入字节长度
            __disable_irq();//关闭中断
            buf0_ready = 0;//buf0就绪标志关闭
            __enable_irq();//使能中断
            IWDG_Boot_Feed(); // ? 新增：写完一包喂一次狗，Flash写入耗时最长
        }
        // ========== 检测buf1就绪 ==========
        else if(buf1_ready == 1)
        {
            __disable_irq();//关闭中断
            uint16_t len = buf1_len;
            __enable_irq();//使能中断
            Flash_write(rx_buf1, len);// 处理缓冲区：把rx_buf1[0 ~ len-1]写入Flash
            receive_all_len += len;//每次写入，记录写入字节长度
            __disable_irq();//关闭中断
            buf1_ready = 0;//buf1就绪标志关闭
            __enable_irq();//使能中断
            IWDG_Boot_Feed(); // ? 新增：写完一包喂一次狗，Flash写入耗时最长
        }
        else if(receive_all_len >= temp_flag.app_len)//接收字节长度达到固件字节长度
        {
            IWDG_Boot_Feed(); // ? 新增：处理最后字节前先喂狗
            bootloader_finish_write(); // 写入最后遗留字节
            if(flash_crc_check(GetSlotUpdateAddr(temp_flag.active_slot), temp_flag.app_len, temp_flag.new_app_crc))//CRC固件校验
            {
                printf("新固件总字节数: %d\r\n",temp_flag.app_len);
                printf("CRC校验通过,升级完成, 执行新APP\r\n");
                temp_flag.state = OTA_VERIFY_OK;
                OTA_Flag_Save();//写入标志位
                IWDG_Boot_Feed(); // ? 新增：写标志后喂狗，保证复位前不超时
                __disable_irq();//关闭中断
                // 内核软件复位：System Reset Request
                NVIC_SystemReset();//复位，执行更新好的app
            }
            else
            {
                printf("%04x",temp_flag.new_app_crc);
                printf("固件CRC校验失败\r\n");
                printf("请重新升级\r\n");
                // 校验失败，清除升级标志，等待重传
                OTA_Flag_Save();
            }
        }
        // ========== 空闲等待（无数据、未完成） ==========
        else
        {
            IWDG_Boot_Feed(); // ? 新增：空闲等待时也定期喂狗
        }
	}

}
                
