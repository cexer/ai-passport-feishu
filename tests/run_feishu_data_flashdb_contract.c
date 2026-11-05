#include "feishu_data.h"
#include "feishu_data_flashdb.h"
#include "feishu_fdb_host.h"
#include <assert.h>
int storage_contract_main(int argc,char **argv);
int main(void)
{
    const feishu_fdb_io_t *io=feishu_fdb_host_reset();
    assert(feishu_fdb_open(io,true)==FEISHU_FDB_OK);feishu_fdb_close();
    assert(feishu_data_flashdb_configure(io)==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_OK);
    assert(feishu_data_flashdb_initialize()==FEISHU_DATA_CONSTRAINT);
    char *args[]={"test","raw-pool","flashdb"};
    return storage_contract_main(3,args);
}
