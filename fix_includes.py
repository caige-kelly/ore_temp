import os
import re

# Site-rooted includes for Ore
MAPPING = {
    r'\.\./\.\./\.\./support/common/hashmap\.h': 'db/storage/hashmap.h',
    r'\.\./\.\./\.\./support/common/vec\.h': 'db/storage/vec.h',
    r'\.\./\.\./\.\./support/common/stringpool\.h': 'db/storage/stringpool.h',
    r'\.\./\.\./common/arena\.h': 'db/storage/arena.h',
    r'\.\./\.\./common/hashmap\.h': 'db/storage/hashmap.h',
    r'\.\./\.\./common/stringpool\.h': 'db/storage/stringpool.h',
    r'\.\./\.\./common/vec\.h': 'db/storage/vec.h',
    r'\.\./\.\./support/common/hashmap\.h': 'db/storage/hashmap.h',
    r'\.\./\.\./support/common/vec\.h': 'db/storage/vec.h',
    r'\.\./\.\./parser/ast\.h': 'parser/ast.h',
    r'\.\./\.\./db/ids/ids\.h': 'db/ids/ids.h',
    r'\.\./ids/ids\.h': 'db/ids/ids.h',
    r'\.\./query/query\.h': 'db/query/query.h',
    r'\.\./\.\./\.\./diag/diag\.h': 'db/diag/diag.h',
    r'\.\./\.\./diag/diag\.h': 'db/diag/diag.h',
    r'#include "\.\./modules/def_map\.h"': '#include "../workspace/def_map.h"',
    r'#include "\.\./eval/const_eval\.h"': '#include "../comptime/const_eval.h"',
}

# More general regex-based replacements for common patterns
REGEX_REPLACEMENTS = [
    (re.compile(r'#include "\.\./\.\./common/'), '#include "db/storage/'),
    (re.compile(r'#include "\.\./common/'), '#include "db/storage/'),
    (re.compile(r'#include "\.\./ids/'), '#include "db/ids/'),
    (re.compile(r'#include "\.\./query/'), '#include "db/query/'),
    (re.compile(r'#include "\.\./\.\./diag/'), '#include "db/diag/'),
    (re.compile(r'#include "\.\./\.\./\.\./diag/'), '#include "db/diag/'),
    (re.compile(r'#include "\.\./\.\./\.\./support/common/'), '#include "db/storage/'),
]

def fix_file(path):
    with open(path, 'r') as f:
        content = f.read()
    
    new_content = content
    for pattern, replacement in MAPPING.items():
        if pattern.startswith('#include'):
             new_content = new_content.replace(pattern, replacement)
        else:
             # Use regex for relative path parts
             regex = re.compile(r'#include "' + pattern + r'"')
             new_content = regex.sub('#include "' + replacement + '"', new_content)
    
    for regex, replacement in REGEX_REPLACEMENTS:
        new_content = regex.sub(replacement, new_content)

    if new_content != content:
        with open(path, 'w') as f:
            f.write(new_content)
        print(f"Fixed {path}")

for root, dirs, files in os.walk('src/sema'):
    for f in files:
        if f.endswith('.c') or f.endswith('.h'):
            fix_file(os.path.join(root, f))
