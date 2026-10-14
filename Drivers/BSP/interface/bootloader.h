#ifndef __BOOTLOADER_H__
#define __BOOTLOADER_H__

#include "uart1.h"

//程序写入位置，A区起始位置，假设B区16k，0x4000  A区（64-16）k 0xC000
//0X08000000~0X08002800 为bootloader区，16kb
//0X08002800~0x08007800 为A分区，APP区，23kb
#define APP_A_START_ADDR 0x08004000U
#define APP_A_END_ADDR 0x08009C00U
//0X08007800~0x0800C800 为B分区，备份区，新固件先存放在这里,23kb
#define APP_B_START_ADDR 0x08009C00U
#define APP_B_END_ADDR 0x0800F800U

#define FLAG_START_ADDR        0x0800F800U  //标志区，2kb，Flash最后2页！
#define FLAG_SIZE              2*1024U

#define STACK_ADDR 0x20000000

#define OTA_MAGIC 0xAA

// 升级状态枚举
typedef enum {
    OTA_IDLE = 0xAA,          // 空闲：App正常，无升级任务
    OTA_DOWNLOADING = 0xBB,   // 正在下载B分区固件
    OTA_VERIFY_OK = 0xCC,     // 下载完成，CRC校验通过，准备切换启动
    OTA_RUN_NEW_FAIL = 0xDD,  // 新App启动失败，触发回滚
} OTA_State_t;

typedef enum {
    SLOT_A = 0,   // A 分区
    SLOT_B = 1    // B 分区
} Slot_t;

//======== OTA标志结构体 ========
typedef struct
{
    OTA_State_t state;
    uint8_t     active_slot;  // 当前正常运行的分区
    uint32_t    new_app_crc;  // 新固件CRC
    uint32_t    app_len;      // 新固件长度
    uint32_t    boot_cnt;     // 新App启动计数（看门狗喂狗判断是否卡死）
} OTA_FlagTypeDef;

//typedef enum
//{
//    STATE_WAIT_CMD,//命令接收
//    STATE_OTA_RECV_FW,//接收固件中
//    STATE_OTA_FINISH//升级完成
//} OTA_StateTypeDef;

// 直接映射到Flash标志区地址，只读的时候直接指针访问
#define OTA_FLAG ((OTA_FlagTypeDef *)FLAG_START_ADDR)
//// 函数声明
//HAL_StatusTypeDef OTA_SetFlag(uint8_t need_upgrade, uint8_t recv_finish, uint32_t crc);

uint8_t Flash_erase(void);//擦除flash

void Flash_write(uint8_t *buf, uint16_t len);//写入flash

void bootloader_jump_to_app(uint32_t app_addr,uint32_t end_addr);//跳转APP

void bootloader_finish_write(void);

uint8_t OTA_Flag_Save(void);

uint16_t crc16_ccitt(uint8_t *pBuf, uint32_t len);

uint8_t flash_crc_check(uint32_t start_addr, uint32_t total_len, uint16_t target_crc);

void Flash_Read(uint32_t src_addr, volatile void *dst, uint32_t len);

void IWDG_Boot_Init(void);

void IWDG_Boot_Feed(void);

/* 根据当前运行分区号获取起始地址 */
uint32_t GetSlotStartAddr(uint8_t slot);
/* 根据当前运行分区号获取结束地址 */
uint32_t GetSlotEndAddr(uint8_t slot);

uint32_t GetSlotUpdateAddr(uint8_t slot);/* 根据当前活动分区获取另一个需要升级目标分区起始地址 */

uint32_t GetSlotUpdateEndAddr(uint8_t slot);/* 根据当前活动分区获取另一个需要升级目标分区结束地址 */

#endif
