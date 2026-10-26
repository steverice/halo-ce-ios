#!/usr/bin/env python3
"""Embed the guest's immutable code as signed Mach-O text, with separate data and a symbol table.

The symbol table (guest-symbols.bin, in the app's __TEXT,__halosyms) names the guest's functions for
the host's crash report (port/ios/host/host_crash.c); guest-symbols.txt is the same table as text,
which tools/guest_symbolicate.py reads to name the guest frames of an Apple crash report."""
import argparse
import hashlib
import struct
from pathlib import Path


SYMBOLS_MAGIC=b'VGSYM1\0\0'


def sections(data):
    """the ELF's section headers, as (name, type, flags, address, offset, size, link, info, align, entsize)"""
    shoff=struct.unpack_from('<Q',data,40)[0]
    entsize,count,_=struct.unpack_from('<HHH',data,58)
    return [struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*entsize) for i in range(count)]


def executable_range(data):
    """the guest addresses the ELF's executable sections span"""
    spans=[(s[3],s[3]+s[5]) for s in sections(data) if s[2]&4 and s[5]]  # SHF_EXECINSTR
    return min(a for a,_ in spans),max(b for _,b in spans)


def code_symbols(data,start,end):
    """[(address, name)] of the ELF's symbols in its executable sections inside [start, end), sorted by
    address: functions and the labels the assembly conversion leaves untyped, not mapping symbols ($x,
    $d) or local labels (.L); of several at one address, a global one is kept over a local one"""
    headers=sections(data)
    executable={index for index,section in enumerate(headers) if section[2]&4}  # SHF_EXECINSTR
    found={}
    for _,kind,_,_,offset,size,link,_,_,each in headers:
        if kind!=2 or not each:continue  # SHT_SYMTAB
        strings=headers[link]
        for index in range(size//each):
            name_offset,info,_,section,value,_=struct.unpack_from('<IBBHQQ',data,offset+index*each)
            if not name_offset or section not in executable or not start<=value<end or info&0xf not in (0,2):continue
            raw=data[strings[4]+name_offset:data.index(b'\0',strings[4]+name_offset)]
            name=raw.decode('utf-8','replace')
            if name.startswith(('$','.L')):continue
            binding=info>>4
            if value not in found or (binding==1 and found[value][0]!=1):found[value]=(binding,name)
    return sorted((address,name) for address,(_,name) in found.items())


def symbol_table(symbols,start,end,digest):
    """the host's binary form (little-endian): the magic, the count, the names' size in bytes, the code's
    first and last guest addresses and the image's sha256 as 64 hex digits, then count (address, name
    offset) pairs of 32-bit values and the NUL-terminated names"""
    names=b''.join(name.encode()+b'\0' for _,name in symbols)
    table=SYMBOLS_MAGIC+struct.pack('<IIII',len(symbols),len(names),start,end)+digest.encode()
    offset=0
    for address,name in symbols:
        table+=struct.pack('<II',address,offset);offset+=len(name.encode())+1
    return table+names


def embed(source, destination):
    """Validate the ELF load ranges and emit assembly plus layout metadata."""
    data=source.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:
        raise ValueError('Expected little-endian AArch64 ELF64')
    offset=struct.unpack_from('<Q',data,32)[0]
    stride,count=struct.unpack_from('<HH',data,54)
    segments=[]
    for i in range(count):
        kind,flags,start,address,_,size,memory,_=struct.unpack_from('<IIQQQQQQ',data,offset+i*stride)
        if kind!=1:continue
        if start+size>len(data) or size>memory or address+memory>0x100000000:
            raise ValueError('Invalid guest segment bounds')
        segments.append((flags,address,size,memory,data[start:start+size]))
    if len(segments)!=2 or [s[0] for s in segments]!=[5,6]:
        raise ValueError('Guest must contain exactly one RX and one RW load segment')
    code,writable=segments
    if code[1]!=0x88000000 or writable[1]%0x4000 or code[1]+code[3]>writable[1]:
        raise ValueError('Unexpected guest layout or page alignment')
    destination.mkdir(parents=True,exist_ok=True)
    code_bytes=code[4].ljust(writable[1]-code[1],b'\0')
    (destination/'guest-code.bin').write_bytes(code_bytes)
    digest=hashlib.sha256(data).hexdigest()
    start,end=executable_range(data) if struct.unpack_from('<H',data,60)[0] else (code[1],code[1]+code[2])
    symbols=code_symbols(data,start,end) if struct.unpack_from('<H',data,60)[0] else []
    (destination/'guest-symbols.bin').write_bytes(symbol_table(symbols,start,end,digest))
    (destination/'guest-symbols.txt').write_text(
        f'# VISR guest symbols: guest image sha256 {digest}\n# address name (guest addresses; code from 0x{start:x} to 0x{end:x})\n'
        +''.join(f'{address:08x} {name}\n' for address,name in symbols))
    (destination/'guest-data.bin').write_bytes(writable[4])
    (destination/'guest_image.h').write_text(
        '/* Generated guest image layout. */\n#pragma once\n'
        f'#define HALO_GUEST_SHA256 "{digest}"\n'
        f'#define IOS_GUEST_CODE_SIZE {len(code_bytes)}u\n'
        f'#define IOS_GUEST_DATA_ADDRESS 0x{writable[1]:x}u\n'
        f'#define IOS_GUEST_DATA_SIZE {writable[2]}u\n'
        f'#define IOS_GUEST_IMAGE_END 0x{writable[1]+writable[3]:x}u\n'
        'extern const unsigned char halo_guest_code[], halo_guest_data[], halo_guest_symbols[];\n')
    (destination/'guest_image.S').write_text(
        '/* Guest code is covered by the enclosing app code signature. */\n'
        '.section __TEXT,__halocode,regular,pure_instructions\n.p2align 14\n'
        '.globl _halo_guest_code\n_halo_guest_code:\n'
        f'.incbin "{(destination/"guest-code.bin").resolve()}"\n'
        '.section __DATA_CONST,__halodata\n.p2align 3\n'
        '.globl _halo_guest_data\n_halo_guest_data:\n'
        f'.incbin "{(destination/"guest-data.bin").resolve()}"\n'
        '.section __TEXT,__halosyms\n.p2align 3\n'
        '.globl _halo_guest_symbols\n_halo_guest_symbols:\n'
        f'.incbin "{(destination/"guest-symbols.bin").resolve()}"\n')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();embed(args.elf,args.output)
