#! /bin/sh
#======================================================================
#
# format.sh -
#
# Created by liubang on 2023/09/21 23:27
# Last Modified: 2023/09/21 23:27
#
#======================================================================

CLANG_FORMAT='clang-format -i'

dirs=('cpp/tools', 'cpp/features', 'cpp/meta', 'cpp/misc')

for dir in "${dirs[@]}"; do
    find "${dir}" -regex '.*\(\.cpp\|\.h\)$' | while read file; do
        cmd="${CLANG_FORMAT} ${file}"
        echo ${cmd}
        eval ${cmd}
    done
done
