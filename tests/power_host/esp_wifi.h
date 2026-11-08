#include "esp_err.h"
typedef enum {WIFI_PS_NONE,WIFI_PS_MIN_MODEM} wifi_ps_type_t;
esp_err_t esp_wifi_get_ps(wifi_ps_type_t*);esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
