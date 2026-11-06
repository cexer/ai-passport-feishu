// components/bsp/src/bsp_battery.c
// 移植自 trae_card/components/platform/platform_esp32/src/battery_cw2017.c
// AI Passport 520mAh 电池 profile；写入后逐字节读回确认。
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // 版本号,上电应答即代表芯片在位
#define CW_REG_VCELL_H   0x02   // 14bit 电压,V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // 高字节 = 整数百分比;低字节(0x05)= 1/256 %
#define CW_REG_CONFIG    0x08   // 0xF0=睡眠 / 0x30=复位态 / 0x00=正常

static i2c_master_dev_handle_t s_dev;
static bool s_ready;
/* AI Passport 520 mAh cell profile, also used by the local XiaoZhi board port.
 * This is board-specific, never a voltage-to-percent guess. */
static const uint8_t s_profile[80]={
    0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAD, 0xC7, 0xC8, 0xCA, 0xBD, 0xB1, 0xC1, 0x94,
    0x88, 0xD1, 0xBD, 0x97, 0x88, 0x66, 0x56, 0x4A,
    0x3F, 0x33, 0x26, 0x5C, 0x37, 0xD1, 0x27, 0xD8,
    0xCC, 0xB7, 0xCF, 0xB3, 0xB2, 0xAE, 0xA6, 0x9E,
    0x99, 0x97, 0x9B, 0x86, 0x47, 0x1E, 0x17, 0x26,
    0x49, 0x96, 0xD9, 0xE1, 0xDD, 0xDC, 0xD4, 0x59,
    0x00, 0x00, 0x90, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5C,
};


static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

esp_err_t bsp_battery_init(void) {
    if (s_ready) return ESP_OK;

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_CW2017_ADDR,
        .scl_speed_hz    = 100000,
    };
    e = s_dev ? ESP_OK:i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "添加 I2C 设备失败: %s", esp_err_to_name(e)); return e; }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != 0) {
        ESP_LOGW(TAG, "CW2017 未应答 —— 用 bsp_i2c_scan() 确认 0x%02X 是否在线;"
                      "无电量计的板子可忽略本项", BSP_I2C_CW2017_ADDR);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "检测到 CW2017 VERSION=0x%02X", ver);

    uint8_t config=0,flag=0,profile[80];
    if(cw_read(CW_REG_CONFIG,&config,1) || cw_read(0x0b,&flag,1) || cw_read(0x10,profile,sizeof(profile)))return ESP_FAIL;
    bool match=(flag&0x80) && !memcmp(profile,s_profile,sizeof(profile));
    if(!match) {
        if(cw_write(CW_REG_CONFIG,0x30))return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(20));
        if(cw_write(CW_REG_CONFIG,0xf0))return ESP_FAIL;
        for(unsigned i=0;i<sizeof(s_profile);++i)if(cw_write((uint8_t)(0x10+i),s_profile[i]))return ESP_FAIL;
        if(cw_read(0x10,profile,sizeof(profile)) || memcmp(profile,s_profile,sizeof(profile)))return ESP_FAIL;
        if(cw_write(0x0b,(uint8_t)(flag|0x80)))return ESP_FAIL;
    }
    if(!match || config!=0) {
        if(cw_write(CW_REG_CONFIG,0x30))return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(20));
        if(cw_write(CW_REG_CONFIG,0))return ESP_FAIL;
    }
    s_ready=true; /* Configured; later SOC retries must not block the app for five seconds. */
    for(unsigned i=0;i<50;++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if(bsp_battery_soc()>=0) {
            s_ready=true;
            ESP_LOGI(TAG,"BATTERY_READY profile=520mah updated=%u soc=%d mv=%d",(unsigned)!match,bsp_battery_soc(),bsp_battery_mv());
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG,"BATTERY_WAIT soc_not_ready=1");
    return ESP_ERR_TIMEOUT;
}

int bsp_battery_soc(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != 0) return -1;
    int soc = b[0];                       // 高字节即整数百分比
    if (soc > 100) return -1;             // 芯片未就绪时可能读到 0xFF
    return soc;
}

int bsp_battery_mv(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != 0) return -1;
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw * 312.5uV → mV
}
