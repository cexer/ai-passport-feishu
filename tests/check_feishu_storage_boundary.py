"""Keep business storage clients independent from the selected DB engine."""
import re
from pathlib import Path
patterns=[r'#include\s*[<"]sqlite3\.h',r'\bsqlite3_\w+',r'\bSQLITE_\w+',r'\bstore\.db\b',r'\bfeishu_vfs_\w+',
          r'\besp_spiffs_\w+',r'\besp_vfs_spiffs_\w+',r'\besp_partition_\w+',r'\bfeishu_recovery_\w+']
for name in ['main/feishu_cache.c','main/feishu_data.h']:
    source=Path(name).read_text(encoding='utf8')
    for pattern in patterns:
        assert not re.search(pattern,source),f'{name}: backend coupling: {pattern}'
print('Storage boundary: public contract/cache contain no native SQLite API PASS')
