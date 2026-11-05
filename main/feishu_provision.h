#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include "feishu_store.h"

// Owner provisioning is available only while onboarding is waiting for an
// app. The task reads a bounded, versioned line from the USB Serial/JTAG
// console and stores owner-provided credentials in the shared config store.
esp_err_t feishu_provision_start(void);
// First-run Wi-Fi provisioning, using FAP-WIFI/1 instead of FAP-FEISHU/1.
// Neither receiver accepts writes for the other onboarding phase.
esp_err_t feishu_provision_start_wifi(void);
// Optional host-assisted app selection; keep credentials in RAM until user grant.
esp_err_t feishu_provision_start_candidate(feishu_credentials_t *candidate);
// Called only by the onboarding worker while waiting for setup. Persistence
// reuses its existing storage-capable stack; the USB task only frames input.
void feishu_provision_commit_pending(void);
void feishu_provision_stop(void);
bool feishu_provision_credentials_received(void);

