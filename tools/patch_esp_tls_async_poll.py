"""Project-only ESP-IDF 5.5.3 async select recovery; reject other source versions."""
from pathlib import Path
import hashlib,sys
EXPECTED="b8f5a79f0efd0b049e4ae132acbdecf22c6494eff269cb5bc93624a88158f3dc"
def patch(source,output):
 if hashlib.sha256(source.read_bytes()).hexdigest()!=EXPECTED:raise ValueError("Unsupported ESP-TLS source hash")
 text=source.read_text(encoding="utf8")
 anchor="#include <esp_log.h>"
 assert text.count(anchor)==1
 text=text.replace(anchor,anchor+'\n#define FEISHU_TLS_POLL_TRACE(text) ESP_LOGI("feishu_tls_poll", "%s", text)\n#include "feishu_tls_poll.h"',1)
 old="""            if (select(tls->sockfd + 1, &tls->rset, &tls->wset, NULL,
                       cfg->timeout_ms>0 ? &tv : NULL) == 0) {"""
 new="""            int poll_ready = feishu_tls_poll_socket(tls->sockfd, &tls->rset, &tls->wset,
                cfg->timeout_ms>0 ? &tv : NULL, select);
            if (poll_ready < 0) {
                ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_SYSTEM, errno);
                ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_ESP, ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST);
                tls->conn_state = ESP_TLS_FAIL;
                return -1;
            }
            if (poll_ready == 0) {"""
 if text.count(old)!=1:raise ValueError("Unsupported ESP-TLS connect polling contract")
 text=text.replace(old,new,1)
 output.parent.mkdir(parents=True,exist_ok=True);output.write_text(text,encoding="utf8")
if __name__=="__main__":patch(Path(sys.argv[1]),Path(sys.argv[2]))
