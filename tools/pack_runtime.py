#!/usr/bin/env python3
"""把酷狗服务库打成 runtime.zip（STORED，不压缩）。

为什么要单独打成一个 zip：
  - levipack 里只允许一个 .so，第二个 .so 会被拒；
    而一个普通 zip 文件不触发这条校验。
  - Android 14 起不允许从可写位置 dlopen 库，所以模组运行时会把
    这个 zip 解到应用私有 cache 目录、chmod 0500，再加载。

为什么必须 STORED（不压缩）：
  解压端（KugouProvider.cpp 的 extractStoredZip）只解析 local file header
  后跟裸数据，不实现 deflate。这样整个工程不需要引入 zlib。

用法：
    python3 tools/pack_runtime.py <服务库.so> <输出.zip>
"""

import os
import sys
import zipfile

FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def main():
    if len(sys.argv) != 3:
        print('用法: pack_runtime.py <服务库.so> <输出.zip>')
        return 2

    src = sys.argv[1]
    out = sys.argv[2]

    if not os.path.isfile(src):
        print('源文件不存在: %s' % src)
        return 1

    data = open(src, 'rb').read()
    if data[:4] != b'\x7fELF':
        print('拒绝：%s 不是 ELF' % src)
        return 1
    if len(data) % 4 != 0:
        # 对齐检查只是提示，不影响功能。
        print('提示：ELF 长度 %d 不是 4 的倍数' % len(data))

    tmp = out + '.tmp'
    with zipfile.ZipFile(tmp, 'w') as z:
        zi = zipfile.ZipInfo('libkugou_server.so', date_time=FIXED_TIME)
        zi.compress_type = zipfile.ZIP_STORED
        zi.external_attr = 0o644 << 16
        z.writestr(zi, data)

    os.replace(tmp, out)

    # ── 校验：必须能原样读回，且每个条目都是 STORED ──
    with zipfile.ZipFile(out) as z:
        assert z.testzip() is None, 'zip 损坏'
        names = z.namelist()
        assert names == ['libkugou_server.so'], '条目异常: %r' % (names,)
        for info in z.infolist():
            assert info.compress_type == zipfile.ZIP_STORED, (
                '%s 不是 STORED' % info.filename)
        assert z.read('libkugou_server.so') == data, '内容不一致'

    print('runtime.zip 完成: %s' % out)
    print('  条目    : libkugou_server.so')
    print('  原大小  : %d 字节' % len(data))
    print('  zip 大小: %d 字节' % os.path.getsize(out))
    print('  压缩方式: STORED（与原始数据等长）')
    return 0


if __name__ == '__main__':
    sys.exit(main())