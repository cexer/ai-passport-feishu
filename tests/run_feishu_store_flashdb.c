#include "feishu_fdb_host.h"
#include <assert.h>
int store_contract_main(int,char **);
int main(int argc,char **argv)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);
    int rc=store_contract_main(argc,argv);
    feishu_fdb_close();return rc;
}
