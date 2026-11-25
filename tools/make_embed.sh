#!/bin/sh
# 把服务库转成可链接的二进制目标文件（objcopy），供主 so 内嵌。
#
# 用法:
#   sh tools/make_embed.sh <libkugou_server.so> [输出.o]
#
# 默认输出 embed/libkugou_blob.o，CMakeLists 里直接引用这个路径。
# 生成后符号为 _binary_<文件名>_start / _end，
# 本例中即 _binary_libkugou_server_so_start / _end。

set -u

SRC="${1:-}"
OUT="${2:-embed/libkugou_blob.o}"

if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
  echo "用法: sh tools/make_embed.sh <libkugou_server.so> [输出.o]" >&2
  echo "本仓库不包含该服务库，需自行获取。" >&2
  exit 1
fi

mkdir -p "$(dirname "$OUT")" || exit 1

objcopy -I binary -O elf64-littleaarch64 -B aarch64 "$SRC" "$OUT" || exit 1

echo "已生成: $OUT"
nm "$OUT" | grep '_binary_'
echo
echo "文件大小: $(stat -c %s "$SRC") 字节"
echo "现在可以执行: sh build.sh"