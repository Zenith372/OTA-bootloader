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
    /* ---- 等待升级命令的入口:跳转失败时会回到这里 ---- */
    wait_cmd:
    cmd_idx = 0;                                 /* 命令帧接收索引清零 */
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
        /* 走到这里说明"跳转新 APP"返回了 = 新槽无效:
         * 清掉待验证状态、清零启动计数,回到等待窗口(靠下面的 goto) */
        temp_flag.state = OTA_IDLE;
        temp_flag.boot_cnt = 0;
        OTA_Flag_Save();
        goto wait_cmd;
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
        frame_reset();              /* 复位帧解析状态,重新期待第 1 包 */
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
        __HAL_UART_ENABLE_IT(&uart1_handle, UART_IT_IDLE);
        send_ctrl(CTRL_READY);      /* 告诉上位机:擦除完成,可以发固件了 */
        printf("启动DMA接收, ret=%d\r\n", dma_ret);  // 新增：0=启动成功
    }
    else//空闲状态OTA_IDLE || OTA_RUN_FAIL
    {
        /* 优先启动当前活动槽 */
        if(temp_flag.active_slot == SLOT_A) bootloader_jump_to_app(APP_A_START_ADDR,APP_A_END_ADDR);
        else                                bootloader_jump_to_app(APP_B_START_ADDR,APP_B_END_ADDR);
        /* 活动槽无效 -> 再试备份槽 */
        printf("active slot invalid, try backup slot\r\n");
        if(temp_flag.active_slot == SLOT_A) bootloader_jump_to_app(APP_B_START_ADDR,APP_B_END_ADDR);
        else                                bootloader_jump_to_app(APP_A_START_ADDR,APP_A_END_ADDR);
        /* 两个槽都无效 -> 停在 OTA 模式,回到等待窗口继续等升级命令 */
        printf("both slots invalid, stay in OTA mode\r\n");
        goto wait_cmd;
        
    }
    /* ===================== 固件接收主循环 =====================
     * 只做两件事:①把收到的一段数据喂给帧解析器
     *            ②所有包收齐后做最终整片校验 */
    while(1)
    {
        if(buf0_ready == 1)                     /* DMA 缓冲0 有数据了 */
        {
            __disable_irq();
            uint16_t len = buf0_len;
            buf0_ready = 0;                     /* 先清标志,防止重复处理 */
            __enable_irq();
            frame_parser_feed(rx_buf0, len);    /* 交给帧解析器:逐包校验 + 写 Flash + 回 ACK */
        }
        else if(buf1_ready == 1)                /* DMA 缓冲1 有数据了 */
        {
            __disable_irq();
            uint16_t len = buf1_len;
            buf1_ready = 0;
            __enable_irq();
            frame_parser_feed(rx_buf1, len);
        }
        else if(g_all_packets_done)             /* 所有包已收完 -> 最终校验 */
        {
            bootloader_finish_write();          /* 补写最后遗留的半个字节 */
            if(flash_crc_check(GetSlotUpdateAddr(temp_flag.active_slot),
                               temp_flag.app_len, temp_flag.new_app_crc))
            {
                send_ctrl(CTRL_OK);             /* 告诉上位机:升级成功 */
                printf("OTA ok, CRC32 pass, jump to new APP\r\n");
                temp_flag.state = OTA_VERIFY_OK;/* 标记"待验证",下次上电启动新固件 */
                OTA_Flag_Save();
                IWDG_Boot_Feed();
                __disable_irq();
                NVIC_SystemReset();             /* 复位,准备运行新固件 */
            }
            else
            {
                send_ctrl(CTRL_FAIL);           /* 告诉上位机:升级失败 */
                printf("OTA fail, CRC32 mismatch\r\n");
                g_all_packets_done = 0;         /* 清标志,等待重新升级 */
            }
        }
        IWDG_Boot_Feed();                       /* 主循环喂狗 */
    }

}
                
