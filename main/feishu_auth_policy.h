#pragma once

#include <stdbool.h>

// HTTP permission failures and malformed responses are not proof of revocation.
bool feishu_auth_access_rejected(int http_status, int api_code);
bool feishu_auth_refresh_revoked(int http_status, const char *oauth_error);
bool feishu_auth_app_rejected(int http_status,const char *oauth_error);
