#ifndef FEISHU_PROBE_FAL_CFG_H
#define FEISHU_PROBE_FAL_CFG_H
extern const struct fal_flash_dev probe_flash;
#define FAL_FLASH_DEV_TABLE { &probe_flash }
#define FAL_PART_HAS_TABLE_CFG
#define FAL_PART_TABLE { {FAL_PART_MAGIC_WORD, "pool", "probe-flash", 0, 640*1024, 0} }
#endif
