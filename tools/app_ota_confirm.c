/* =====================================================================
 *  app_ota_confirm.c  ——  APP 端"升级确认"参考实现
 *  ---------------------------------------------------------------------
 *  重要:本文件是给你【APP 工程】用的参考代码,不要加入 Bootloader 工程。
 *        Bootloader 工程里不需要它(编译进去只会白白占 Flash)。
 *
 *  为什么需要它(这是 OTA 回滚闭环缺失的另一半):
 *    新固件升级成功 -> 复位 -> 启动之后,APP 必须主动"报到":
 *        state       -> OTA_IDLE       告诉 bootloader: 我活了,别再回滚我
 *        active_slot -> 本 APP 所在槽   把活动槽切到我这里,A/B 才能交替
 *        boot_cnt    -> 0
 *    否则会出现两个后果:
 *      (1) 一个完全正常的新固件,每次上电 boot_cnt 都会 +1,
 *          第 3 次上电就会被误判"启动失败"而回滚到旧固件;
 *      (2) active_slot 永远指向旧槽,下一次升级会去擦【正在运行的槽】,
 *          A/B 双槽语义彻底错乱。
 *
 *  怎么用:
 *    1) 把本文件加入你的 APP 工程(注意 include 路径按你的工程改);
 *    2) 在 APP 里"关键初始化都完成、确定能跑"之后再调用一次:
 *           OTA_ConfirmIfNeeded(MY_SLOT);
 *       其中 MY_SLOT 按本 APP 的链接起始地址填:
 *           APP 从 0x08004000 开始 -> SLOT_A(0)
 *           APP 从 0x08009C00 开始 -> SLOT_B(1)
 *    3) APP 必须定期喂 IWDG(IWDG 一旦启动无法关闭),
 *       否则 2 秒就被复位,会被当成"启动失败"。
 *
 *  对齐要求(必须和 Bootloader 完全一致,否则读到的字段全错):
 *    - 标志区地址 FLAG_START_ADDR = 0x0800F800
 *    - 结构体 OTA_FlagTypeDef 的字段顺序与类型(最好直接从 bootloader.h 复制)
 * ===================================================================== */

#include "stm32f1xx_hal.h"      /* 按你的 APP 工程实际头文件调整 */

/* ---------------- 必须与 bootloader.h 保持一致 ---------------- */
#define FLAG_START_ADDR   0x0800F800U
#define OTA_FLAG_PAGES    2U            /* 标志区 2 页 */
#define OTA_FLAG_PGSIZE   1024U         /* STM32F103C8 中容量:1KB/页 */

typedef enum {
    OTA_IDLE         = 0xAA,
    OTA_DOWNLOADING  = 0xBB,
    OTA_VERIFY_OK    = 0xCC,
    OTA_RUN_NEW_FAIL = 0xDD,
} OTA_State_t;

typedef enum {
    SLOT_A = 0,
    SLOT_B = 1
} Slot_t;

typedef struct
{
    OTA_State_t state;
    uint8_t     active_slot;
    uint32_t    new_app_crc;
    uint32_t    app_len;
    uint32_t    boot_cnt;
} OTA_FlagTypeDef;

/* ---------------- 内部工具 ---------------- */
static void ota_flag_read(OTA_FlagTypeDef *f)
{
    const uint8_t *src = (const uint8_t *)FLAG_START_ADDR;
    uint8_t       *dst = (uint8_t *)f;
    uint32_t i;
    for (i = 0; i < sizeof(OTA_FlagTypeDef); i++)
        dst[i] = src[i];
}

static uint8_t ota_flag_save(const OTA_FlagTypeDef *f)
{
    FLASH_EraseInitTypeDef er;
    uint32_t page_err = 0xFFFFFFFFU;
    const uint16_t *src = (const uint16_t *)f;
    uint32_t addr = FLAG_START_ADDR;
    uint32_t i;

    HAL_FLASH_Unlock();

    for (i = 0; i < OTA_FLAG_PAGES; i++)
    {
        er.TypeErase   = FLASH_TYPEERASE_PAGES;
        er.Banks       = FLASH_BANK_1;
        er.PageAddress = FLAG_START_ADDR + i * OTA_FLAG_PGSIZE;
        er.NbPages     = 1;
        if (HAL_FLASHEx_Erase(&er, &page_err) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
    }

    for (i = 0; i < sizeof(OTA_FlagTypeDef) / 2; i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr, src[i]) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return 0;
        }
        addr += 2;
    }

    HAL_FLASH_Lock();
    return 1;
}

/* ---------------- 给 APP 调用的接口 ---------------- */
/* 在 APP 启动成功、关键外设初始化完成后调用一次 */
void OTA_ConfirmIfNeeded(uint8_t my_slot)
{
    OTA_FlagTypeDef f;

    ota_flag_read(&f);

    if (f.state == OTA_VERIFY_OK)       /* 说明我是刚升级上来的那份固件 */
    {
        f.state       = OTA_IDLE;       /* 报到:我活了 */
        f.active_slot = my_slot;        /* 把活动槽切到我这里 */
        f.boot_cnt    = 0;
        ota_flag_save(&f);
    }
}
