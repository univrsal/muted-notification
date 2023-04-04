#!/bin/bash
#grep -rl 'Copyright 2023' ./projects | xargs sed -i 's/Copyright 2023/Copyright 2024/g'
find ./src -iname *.h* -o -iname *.c* | xargs clang-format -i -verbose
