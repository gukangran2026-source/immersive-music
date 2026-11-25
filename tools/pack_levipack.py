#!/usr/bin/env python3
"""把一个 preload-native 模组打成 levipack。

levipack 就是一个 zip，里面放：
    <ModDir>/manifest.json
    <ModDir>/libxxx.so

包内目录名会成为宿主认定的模组 ID（见 README 里「模组 ID 必须等于目录名」），
所以 <ModDir> 用什么名字，玩家装出来的目录就叫什么。

用法：
    python3 pack_levipack.py --so build/final/libmusicmod.so \
                             --manifest manifest.json \
                             --dir MusicMod \
                             --out dist/MusicMod.levipack

校验：包内顶层只能出现一个 .so，权限统一 0644。
"""
import argparse
import os
import sys
import zipfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--so', required=True, help='主模组 so 路径')
    ap.add_argument('--manifest', required=True, help='manifest.json 路径')
    ap.add_argument('--dir', default='MusicMod', help='包内目录名，默认 MusicMod')
    ap.add_argument('--out', required=True, help='输出的 .levipack 路径')
    args = ap.parse_args()

    for p in (args.so, args.manifest):
        if not os.path.isfile(p):
            sys.exit('找不到文件: %s' % p)

    so_name = os.path.basename(args.so)
    if not so_name.endswith('.so'):
        sys.exit('--so 必须是 .so 文件')

    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    entries = [
        (os.path.join(args.dir, so_name), args.so),
        (os.path.join(args.dir, 'manifest.json'), args.manifest),
    ]

    with zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED) as zf:
        # 目录条目，权限带目录位
        d = zipfile.ZipInfo(args.dir + '/')
        d.external_attr = (0o644 << 16) | 0x10
        zf.writestr(d, b'')
        for arc, src in entries:
            with open(src, 'rb') as f:
                data = f.read()
            zi = zipfile.ZipInfo(arc)
            zi.external_attr = 0o644 << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(zi, data)
            print('  + %s  %d 字节' % (arc, len(data)))

    # 自检：顶层只允许一个 so
    with zipfile.ZipFile(args.out) as zf:
        sos = [n for n in zf.namelist()
               if n.endswith('.so') and n.count('/') == 1]
        if len(sos) != 1:
            os.remove(args.out)
            sys.exit('包内 so 数量为 %d，应为 1: %s' % (len(sos), sos))

    print('完成: %s  %d 字节' % (args.out, os.path.getsize(args.out)))


if __name__ == '__main__':
    main()