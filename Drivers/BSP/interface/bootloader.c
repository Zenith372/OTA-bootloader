#include "bootloader.h"
#include <stdio.h>
#include <string.h>
#include "uart1.h"

extern UART_HandleTypeDef uart1_handle;
extern DMA_HandleTypeDef dma_handle;


uint32_t flash_write_offset = 0;//记录当前写入程序的偏移量
uint8_t last_byte = 0;  //遗留字节
uint8_t last_byte_flag = 0;  //表示是否有遗留字节
uint8_t active_buf = 0;  //0代表buf0正在写入，1代表buf1正在写入
uint8_t rx_buf0[UART1_RX_BUF_SIZE];  // 缓冲区0，SRAM中独立地址
uint8_t rx_buf1[UART1_RX_BUF_SIZE];  // 缓冲区1，SRAM中独立地址 /*这俩也是UART1接收缓冲区 *///由dma搬运到这里
volatile uint8_t buf0_ready;//buf0缓冲区就绪（缓冲区已满）
volatile uint16_t buf0_len;//buf0缓冲区接收到的字节长度
volatile uint8_t buf1_ready;//buf1缓冲区就绪（缓冲区已满）
volatile uint16_t buf1_len;//buf1缓冲区接收到的字节长度
extern volatile uint32_t receive_all_len;  //新写入固件总长度

// 命令解析相关
//例如：A55A0100001184D5817B74
//- OTA 开始指令：`A5 5A 01 00 00 11 84 D5 81 7B 74`    共11个字节
#define CMD_LEN 11 //命令帧总长度
uint8_t cmd_buf[CMD_LEN];//命令帧缓冲区
uint8_t cmd_idx = 0;//已接收到的命令帧字节数
uint8_t ch_recv= 0;//单字节接收命令帧
volatile uint8_t  g_is_full_packet = 0;//当前包是否是满包
volatile OTA_FlagTypeDef temp_flag;
typedef void (*pFunc)(void);

volatile uint8_t a=0;//调试专用标志位

//原理（老 HAL 库 F103 可用）
//1.调用 `HAL_UART_Receive_DMA(&huart1, buf, 256)` 启动 DMA 搬运
//2.手动开启串口 IDLE 中断：`__HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);`
//3.在 `USART1_IRQHandler` 里面检测 IDLE 标志，**空闲中断触发时，读取 NDTR 寄存器算出本次收到多少字节**
void USART1_IRQHandler(void)//
{
    if (__HAL_UART_GET_FLAG(&uart1_handle, UART_FLAG_IDLE) != RESET)        /* 获取接收空闲中断标志位是否被置位 */
    {	
        //清除空闲中断，否则会一直触发空闲中断
        __HAL_UART_CLEAR_IDLEFLAG(&uart1_handle);
        if(g_is_full_packet)
        {
            g_is_full_packet = 0;
            return;
        }
        
        //停止 DMA 传输，防止干扰
        HAL_UART_DMAStop(&uart1_handle);
        for(volatile uint16_t i=0;i<200;i++);
        //获取接收到的数据长度
        uint16_t recv_len = UART1_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(&dma_handle); //已经接收到的数据长度
        uint32_t remain_bytes = temp_flag.app_len - receive_all_len;
        if(recv_len > remain_bytes)
        {
            recv_len = (uint16_t)remain_bytes; // 强制裁剪到152字节
        }
        if(recv_len > 0 && (recv_len != UART1_RX_BUF_SIZE)) // 有剩余数据
        {
            if(active_buf == 0)//buf0空闲
            {
                buf0_len = recv_len;
                buf0_ready = 1;
            }
            else
            {
                buf1_len = recv_len;
                buf1_ready = 1;
            }
        }
        if(receive_all_len < temp_flag.app_len) // 还没接收完
        {
            // 切换 DMA 到另一个缓冲区
            if(active_buf == 0)
            {
                HAL_UART_Receive_DMA(&uart1_handle, rx_buf1, UART1_RX_BUF_SIZE);
                active_buf = 1;
            }
            else
            {
                HAL_UART_Receive_DMA(&uart1_handle, rx_buf0, UART1_RX_BUF_SIZE);
                active_buf = 0;
            }
        }
        // 收到最后半包，本次OTA接收结束，不再重启DMA
    }
    HAL_UART_IRQHandler(&uart1_handle);
}

void DMA1_Channel5_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&dma_handle);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if(huart->Instance == USART1)
    {
        if(__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE) != RESET)
        {
            printf("ORE Overflow\r\n"); // 溢出错误 = 处理不过来丢包
            //F1必须读SR再读DR，dummy仅占位，不能优化掉
            volatile uint16_t dummy;
            dummy = huart->Instance->SR;
            dummy = huart->Instance->DR;
        
            //清除错误状态，重启接收
            huart->ErrorCode = HAL_UART_ERROR_NONE;
            // 用 DMA 版本重启，指向当前活跃缓冲区
            if(active_buf == 0)
            {
                HAL_UART_Receive_DMA(&uart1_handle, rx_buf0, UART1_RX_BUF_SIZE);
            }
            else
            {
                HAL_UART_Receive_DMA(&uart1_handle, rx_buf1, UART1_RX_BUF_SIZE);
            }
        }
    }
}

void bootloader_jump_to_app(uint32_t app_addr,uint32_t end_addr)
{
    //校验1
    //栈顶地址的值
    uint32_t app_stack_ptr = *(volatile uint32_t*)(app_addr);
    uint32_t app_reset_handle = *(volatile uint32_t*)(app_addr + 4);

    //1.1校验栈顶地址
    if((app_stack_ptr & 0xFFFF0000) != STACK_ADDR)
    {
        printf("stack addr error\n");
        return;
    }
    
    //1.2校验复位中断地址
    if(app_reset_handle <= app_addr || app_reset_handle >= end_addr)
    {
        printf("%08X  %08X  %08X\n",app_addr,end_addr, app_reset_handle);
        printf("reset handle error\n");
        return;
    }

    HAL_NVIC_DisableIRQ(DMA1_Channel5_IRQn);
    HAL_NVIC_DisableIRQ(USART1_IRQn);
    
    //关闭systick
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    //注销HAL库
    //HAL_DeInit();
    
    //2.1关闭中断
    __disable_irq();
    //2.2设置堆栈指针
    __set_MSP(app_stack_ptr);
    //2.3重定向中断向量表
    SCB->VTOR = app_addr;
    __DSB();//内存屏障
    __ISB();
    
    /* 跳转前喂一次狗，给App预留充足启动时间 */
    IWDG_Boot_Feed();
    //2.4跳转到A程序复位中断
    pFunc jump_to_app = (pFunc)app_reset_handle;
    jump_to_app();//跳转之后的内容是执行不到的
}

void Flash_write(uint8_t *buf, uint16_t len)//写入flash
{
    HAL_StatusTypeDef ret;
    uint16_t i = 0;
    uint16_t data16;
    HAL_FLASH_Unlock();
    if(last_byte_flag)
    {
        data16 = last_byte | buf[0] << 8;
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, GetSlotUpdateAddr(temp_flag.active_slot) + flash_write_offset, data16);
        
        flash_write_offset += 2;
        i = 1;
        last_byte_flag = 0;
    }

    // 两两拼接写入
    for(; i + 1 < len; i += 2)
    {
        data16 = buf[i] | (buf[i+1] << 8);
        ret = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, GetSlotUpdateAddr(temp_flag.active_slot) + flash_write_offset, data16);
        if(ret != HAL_OK)
        {   
        printf("Flash write err, offset:%d ret:%d\r\n",flash_write_offset,ret);
        }
        flash_write_offset += 2;
    }

    if(i < len)//处理遗留字节后剩下的为奇数长度，则会剩下一个字节
    {
        last_byte = buf[i];
        last_byte_flag = 1;//有遗留
    }
    HAL_FLASH_Lock();
}   

/* 擦除升级目标区（非活动区）。返回 1=成功，0=失败 */
uint8_t Flash_erase(void)//擦除然后调用写入flash
{
    // 一次性擦除整个APP区域
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase_init;
    uint32_t start = GetSlotUpdateAddr(temp_flag.active_slot);
    uint32_t end   = GetSlotUpdateEndAddr(temp_flag.active_slot);
    uint32_t page_cnt = (end - start) / FLASH_PAGE_SIZE;
    uint32_t page_error = 0xFFFFFFFFU;
    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks = FLASH_BANK_1;
     /* 1. 边界硬校验：擦错范围 = 抹掉 bootloader = 永久变砖 */
    if (end <= start || start < APP_A_START_ADDR || end > FLAG_START_ADDR)
    {
        printf("erase range err %08X-%08X\r\n", start, end);
        return 0;
    }

    if (page_cnt == 0) return 0;//需要擦除页为0

    for(uint32_t i = 0; i < page_cnt; i++)//每次只擦一页
    {
        IWDG_Boot_Feed(); /* 擦前喂 */
        erase_init.PageAddress = GetSlotUpdateAddr(temp_flag.active_slot) + i * FLASH_PAGE_SIZE;
        erase_init.NbPages = 1; //每次只擦1页
        HAL_StatusTypeDef ret = HAL_FLASHEx_Erase(&erase_init, &page_error);
        if (ret != HAL_OK || page_error != 0xFFFFFFFFU)
        {
            printf("erase fail pg%u addr=%08X st=%d\r\n",
                   i, page_error, (int)ret);
            HAL_FLASH_Lock();            /* 失败也必须上锁 */
            return 0;                     /* 绝不带病往下写 */
        }
        IWDG_Boot_Feed(); //✅擦完一页立刻喂狗
    }
    HAL_FLASH_Lock();
    return 1;
}
// 升级完成后调用：写入最后遗留的奇数字节
void bootloader_finish_write(void)
{
    if(last_byte_flag)
    {
        HAL_FLASH_Unlock();
        uint16_t data16 = last_byte | 0xFF00; // 补0xFF写入最后半字
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, GetSlotUpdateAddr(temp_flag.active_slot) + flash_write_offset, data16);
        flash_write_offset += 2;
        HAL_FLASH_Lock();
        last_byte_flag = 0;
    }
}

// 单字节接收/DMA 接收完成（缓冲区填满） 中断回调
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if(huart->Instance != USART1) return;
    // ===== 模式1：DMA固件大包接收 =====
    if(temp_flag.state == OTA_DOWNLOADING && receive_all_len < temp_flag.app_len)
    {
        if(huart->pRxBuffPtr == rx_buf0)
        {
            // 标记：这是满包，后面到来的IDLE中断直接跳过
            g_is_full_packet = 1;

            buf0_len = UART1_RX_BUF_SIZE;
            buf0_ready = 1;
            // 切换DMA到buf1
            HAL_UART_Receive_DMA(&uart1_handle, rx_buf1, UART1_RX_BUF_SIZE);
            active_buf = 1;  // ✅ 同步更新：当前DMA目标是buf1
        }
        else if(huart->pRxBuffPtr == rx_buf1)
        {
            // 标记：这是满包，后面到来的IDLE中断直接跳过
            g_is_full_packet = 1;
            buf1_len = UART1_RX_BUF_SIZE;
            buf1_ready = 1;
            // 切换DMA到buf0
            HAL_UART_Receive_DMA(&uart1_handle, rx_buf0, UART1_RX_BUF_SIZE);
            active_buf = 0;  // ✅ 同步更新：当前DMA目标是buf0
        }
    }
    // ===== 模式2：单字节指令接收模式 =====
    else
    {
        uint8_t ch = ch_recv;
        cmd_buf[cmd_idx++] = ch;
        //printf("%d\r\n",cmd_idx);
        if(cmd_idx >= CMD_LEN)
        {
            a++;
            // 1. 先匹配帧头
            if(cmd_buf[0] == 0xA5 && cmd_buf[1] == 0x5A && cmd_buf[2] == 0x01)
            {
                // 2. 计算前7字节的CRC
                uint16_t calc_crc = crc16_ccitt(cmd_buf, 9);
                // 3. 提取收到的CRC（低字节在前，高字节在后）
                uint16_t recv_crc = cmd_buf[9] | (cmd_buf[10] << 8);
                //printf("calc_crc:0x%04X, recv_crc:0x%04X\r\n",calc_crc,recv_crc);
                // 4. CRC校验通过才执行命令
                if(calc_crc == recv_crc)
                {
                    //获取到升级命令，切换为升级下载状态
                    temp_flag.state = OTA_DOWNLOADING;
                    // 提取4字节总长度（大端示例）
                    temp_flag.app_len = (cmd_buf[3] << 24) |
                                    (cmd_buf[4] << 16) |
                                    (cmd_buf[5] << 8)  |
                                    (cmd_buf[6]);
                    printf("%08x",temp_flag.app_len);
                    //解析固件目标CRC（2字节大端）
                    temp_flag.new_app_crc = ((uint16_t)cmd_buf[7] << 8) | cmd_buf[8];
                }
                else
                {
                    // CRC校验失败，丢弃这帧，可选打印错误
                    printf("cmd CRC error\r\n");
                }
            }
            cmd_idx = 0;// 清空索引，等待下一帧
        }
        HAL_UART_Receive_IT(&uart1_handle, &ch_recv, 1);
    }
}

// 假设 FLAG_START_ADDR 所在的扇区是 FLASH_SECTOR_X
uint8_t OTA_Flag_Save(void)//OTA_Flag_Save：把 RAM temp_flag 保存到 Flash（只有 OTA 关键节点调用）
{
    HAL_StatusTypeDef ret;
    __disable_irq();
    // 4. 擦除 Flash 扇区
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase_init;
    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;//擦除类型为页擦除
    erase_init.Banks = FLASH_BANK_1;//擦除的页所在的bank(FLASH有几块)
    uint32_t page_error = 0;//不显示擦拭失败的页地址信息

    for(uint32_t i = 0; i < 2; i++)//每次只擦一页,标志区一共2页
    {
        erase_init.PageAddress = FLAG_START_ADDR + i * FLASH_PAGE_SIZE;//擦除的页地址
        erase_init.NbPages = 1; //每次只擦1页
        HAL_FLASHEx_Erase(&erase_init, &page_error);
        IWDG_Boot_Feed(); //擦完一页立刻喂狗
    }

    ret = HAL_FLASHEx_Erase(&erase_init, &page_error);
    if(ret != HAL_OK)
    {
        HAL_FLASH_Lock();
        __enable_irq();
        return 0;
    }
    // 5. 将 RAM 里的值写回 Flash（注意对齐，通常按字或半字写入）
    uint16_t *p_data = (uint16_t *)&temp_flag;
    uint32_t addr = FLAG_START_ADDR;
    for(uint32_t i = 0; i < sizeof(OTA_FlagTypeDef)/2; i++)
    {
        HAL_StatusTypeDef ret = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, p_data[i]);
        if(ret != HAL_OK)
        {
            HAL_FLASH_Lock();
            __enable_irq();
            return 0;
        }
        addr += 2;
    }
    HAL_FLASH_Lock();
    __enable_irq();
    return 1;
}

// 计算CRC16-CCITT
// pBuf: 数据缓冲区指针
// len: 数据长度
// 返回: 16位CRC结果
uint16_t crc16_ccitt(uint8_t *pBuf, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    while(len--)
    {
        crc ^= (uint16_t)*pBuf++ << 8;
        for(uint8_t i = 0; i < 8; i++)
        {
            if(crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// 校验APP区域Flash的CRC16
// start_addr: APP起始地址
// total_len: 固件总长度
// target_crc: 目标CRC值
// 返回: 1=校验通过，0=校验失败
uint8_t flash_crc_check(uint32_t start_addr, uint32_t total_len, uint16_t target_crc)
{
    uint16_t calc_crc = 0xFFFF;
    uint8_t *p_flash = (uint8_t *)start_addr;
    
    for(uint32_t i = 0; i < total_len; i++)
    {
        uint8_t data = p_flash[i];
        calc_crc ^= (uint16_t)data << 8;
        for(uint8_t j = 0; j < 8; j++)
        {
            if(calc_crc & 0x8000)
                calc_crc = (calc_crc << 1) ^ 0x1021;
            else
                calc_crc <<= 1;
        }
    }
    printf("%04x",calc_crc);
    return (calc_crc == target_crc) ? 1 : 0;
}

/**
 * @brief  读取Flash指定地址到RAM缓冲区
 * @param  src_addr: Flash源地址(0x08xxxxxx)
 * @param  dst: RAM目标缓冲区指针
 * @param  len: 读取字节长度
 * @retval 无
 */
void Flash_Read(uint32_t src_addr, volatile void *dst, uint32_t len)
{
    uint8_t *p_src = (uint8_t *)src_addr;
    uint8_t *p_dst = (uint8_t *)dst;

    for(uint32_t i = 0; i < len; i++)
    {
        p_dst[i] = p_src[i];
    }
}

IWDG_HandleTypeDef hiwdg;
/* IWDG初始化：超时时间≈2秒（LSI 40kHz）
 * 40000 / 64 = 625Hz，重装载1250 → 2秒溢出复位
 */
void IWDG_Boot_Init(void)
{
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_64;
    hiwdg.Init.Reload = 1250;
    HAL_IWDG_Init(&hiwdg);
}

/* Bootloader内喂狗，防止Flash擦写过程中超时复位 */
void IWDG_Boot_Feed(void)
{
    HAL_IWDG_Refresh(&hiwdg);
}

uint32_t GetSlotUpdateAddr(uint8_t slot)/* 根据当前活动分区获取另一个需要升级目标分区起始地址 */
{
    return (slot == SLOT_A) ? APP_B_START_ADDR : APP_A_START_ADDR;
}

uint32_t GetSlotUpdateEndAddr(uint8_t slot)/* 根据当前活动分区获取另一个需要升级目标分区结束地址 */
{
    return (slot == SLOT_A) ? APP_B_END_ADDR : APP_A_END_ADDR;
}

uint32_t GetSlotStartAddr(uint8_t slot)/* 获取当前活动分区起始地址 */
{
    return (slot == SLOT_A) ? APP_A_START_ADDR : APP_B_START_ADDR;
}

uint32_t GetSlotEndAddr(uint8_t slot)/* 获取当前活动分区结束地址 */
{
    return (slot == SLOT_A) ? APP_A_END_ADDR : APP_B_END_ADDR;
}

