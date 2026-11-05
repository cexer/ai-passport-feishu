/* Load SQLite's initialized state normally, before the SDK's GP data region.
 * This changes neither total RAM ownership nor the firmware partition address. */
SECTIONS
{
  .feishu_sqlite.data : ALIGN(4)
  {
    _feishu_sqlite_data_start = ABSOLUTE(.);
    *libfeishu_sqlite.a:sqlite3.*(.data .data.*)
    _feishu_sqlite_data_end = ABSOLUTE(.);
  }
}
INSERT BEFORE .dram0.data;
