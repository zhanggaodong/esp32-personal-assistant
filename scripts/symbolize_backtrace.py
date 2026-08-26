#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""symbolize_backtrace.py — 无工具链的 ESP32 panic backtrace 符号化。

直接解析 ELF 的 .symtab（纯标准库，不需要 xtensa-elf-addr2line），
把 Guru Meditation 的 Backtrace 地址翻译成 函数名+偏移。

用法（任选其一）：
  1) 从 CI 的 personal_assistant_jiuchuan-s3-headless_<sha>_symbols 压缩包里
     解出 xiaozhi.elf，然后：
       python scripts/symbolize_backtrace.py xiaozhi.elf backtrace.txt
  2) 直接给地址：
       python scripts/symbolize_backtrace.py xiaozhi.elf 0x420f7732 0x4038da0a

说明：地址必须与固件来自同一次构建（对比串口日志里的
"ELF file SHA256" 与构建产物是否同批）。ROM 地址(0x40000000~0x4036FFFF)
不在应用符号表内，只能标注为 ROM。
"""
import bisect
import os
import re
import struct
import sys

SHT_SYMTAB = 2
STT_FUNC = 2

FLASH_EXEC_LO, FLASH_EXEC_HI = 0x42000000, 0x43000000
IRAM_EXEC_LO, IRAM_EXEC_HI = 0x40370000, 0x40400000
ROM_LO, ROM_HI = 0x40000000, 0x40370000


def parse_func_symbols(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        raise SystemExit('%s 不是 ELF 文件' % path)
    if data[4] != 1 or data[5] != 1:
        raise SystemExit('仅支持 ELF32 小端（Xtensa 固件）')
    e_shoff, = struct.unpack_from('<I', data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2E)

    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (sh_name, sh_type, _flags, _addr, sh_offset, sh_size,
         sh_link, _info, _align, sh_entsize) = struct.unpack_from('<10I', data, off)
        secs.append({
            'name_off': sh_name, 'type': sh_type, 'offset': sh_offset,
            'size': sh_size, 'link': sh_link, 'entsize': sh_entsize,
        })

    def cstr(strtab_off, name_off):
        start = strtab_off + name_off
        end = data.index(b'\x00', start)
        return data[start:end].decode('utf-8', errors='replace')

    shstrtab = secs[e_shstrndx]

    symtab = None
    for s in secs:
        if s['type'] == SHT_SYMTAB:
            symtab = s
            break
    if symtab is None:
        raise SystemExit('ELF 里没有 .symtab（可能被 strip），无法符号化')

    strtab = secs[symtab['link']]
    symbols = []
    count = symtab['size'] // 16
    for i in range(count):
        off = symtab['offset'] + i * 16
        st_name, st_value, st_size, st_info, _other, _shndx = struct.unpack_from(
            '<IIIBBH', data, off)
        if st_name == 0 or (st_info & 0xF) != STT_FUNC or st_size == 0:
            continue
        symbols.append((st_value, st_size, cstr(strtab['offset'], st_name)))
    symbols.sort()
    return symbols


def resolve(symbols, addr):
    keys = [s[0] for s in symbols]
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return '<无更近符号>'
    value, size, name = symbols[i]
    tag = '' if addr < value + size else '(最近符号,超出函数体)'
    return '%s+0x%x %s' % (name, addr - value, tag)


def classify(addr):
    if FLASH_EXEC_LO <= addr < FLASH_EXEC_HI:
        return ''
    if IRAM_EXEC_LO <= addr < IRAM_EXEC_HI:
        return '(IRAM)'
    if ROM_LO <= addr < ROM_HI:
        return '(ROM,应用符号表外)'
    return '(非代码地址,跳过)'


def extract_addrs(text):
    addrs = []
    for m in re.finditer(r'0x([0-9a-fA-F]{7,8})', text):
        a = int(m.group(1), 16)
        # 回溯行格式 "PC:SP"，冒号后是栈地址；只取代码段范围
        if FLASH_EXEC_LO <= a < FLASH_EXEC_HI or IRAM_EXEC_LO <= a < IRAM_EXEC_HI \
                or ROM_LO <= a < ROM_HI:
            addrs.append(a)
    return addrs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    elf_path = sys.argv[1]
    rest = sys.argv[2:]

    text = ' '.join(rest)
    if len(rest) == 1 and os.path.isfile(rest[0]):
        with open(rest[0], encoding='utf-8', errors='replace') as f:
            text = f.read()

    symbols = parse_func_symbols(elf_path)
    print('# 已加载 %d 个函数符号 (%s)' % (len(symbols), elf_path))
    addrs = extract_addrs(text)
    if not addrs:
        print('# 输入中未发现可解析的代码地址')
        return
    for a in addrs:
        kind = classify(a)
        if '跳过' in kind:
            continue
        print('0x%08x %s -> %s' % (a, kind, resolve(symbols, a)))


if __name__ == '__main__':
    main()
