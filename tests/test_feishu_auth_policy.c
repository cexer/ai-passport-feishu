#include "feishu_auth_policy.h"

#include <assert.h>

int main(void)
{
    // Missing permission, unavailable upstream, malformed/empty JSON and rate
    // limits must keep the binding. A bare 403 is not a revoked user token.
    assert(!feishu_auth_access_rejected(403, 99991672));
    assert(!feishu_auth_access_rejected(403, 0));
    assert(!feishu_auth_access_rejected(200, 99991672));
    assert(!feishu_auth_access_rejected(200, 0));
    assert(!feishu_auth_access_rejected(429, 0));
    assert(!feishu_auth_access_rejected(503, 99991668));
    assert(!feishu_auth_access_rejected(0, 0));
    assert(feishu_auth_access_rejected(401, 0));
    assert(feishu_auth_access_rejected(200, 20005));
    assert(feishu_auth_access_rejected(200, 20006));
    assert(feishu_auth_access_rejected(200, 99991668));
    assert(!feishu_auth_refresh_revoked(503, "invalid_grant"));
    assert(!feishu_auth_refresh_revoked(429, "invalid_grant"));
    assert(!feishu_auth_refresh_revoked(400, "invalid_client"));
    assert(!feishu_auth_refresh_revoked(400, "invalid_scope"));
    assert(!feishu_auth_refresh_revoked(400, ""));
    assert(!feishu_auth_refresh_revoked(400, 0));
    assert(feishu_auth_refresh_revoked(400, "invalid_grant"));
    assert(feishu_auth_app_rejected(400,"invalid_client"));
    assert(feishu_auth_app_rejected(401,"invalid_client"));
    assert(!feishu_auth_app_rejected(503,"invalid_client"));
    assert(!feishu_auth_app_rejected(429,"invalid_client"));
    assert(!feishu_auth_app_rejected(400,"invalid_scope"));
    return 0;
}
