#include "feishu_auth_policy.h"

#include <string.h>

bool feishu_auth_access_rejected(int http_status, int api_code)
{
    if (http_status == 401) return true;
    if (http_status < 200 || http_status >= 500) return false;
    return api_code == 20005 || api_code == 20006 ||
           api_code == 99991663 || api_code == 99991664 ||
           api_code == 99991665 || api_code == 99991668;
}

bool feishu_auth_refresh_revoked(int http_status, const char *oauth_error)
{
    // Only an explicit refresh-grant rejection permits local token removal.
    // invalid_client / invalid_scope require configuration repair, not unbinding.
    return http_status == 400 && oauth_error != NULL &&
           strcmp(oauth_error, "invalid_grant") == 0;
}
bool feishu_auth_app_rejected(int status,const char *error)
{ return (status==400 || status==401) && error && !strcmp(error,"invalid_client"); }
