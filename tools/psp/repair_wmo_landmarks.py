"""Add bounded landmark metadata using the original client's MODN/MODD identities.
Existing collision/mesh/texture offsets and the world stamp are preserved.
"""
import argparse
from pathlib import Path
import shutil
import struct

HEROES={'statuealleria','statuedanath','statuekhadgar','statuekurdran','statueturalyon'}
def hero(name):
    name=name.lower().replace('\\','/')
    stem,ext=name.rsplit('.',1) if '.' in name else ('','')
    return ext in ('m2','mdx') and stem.startswith('world/generic/human/passive doodads/statues/') and stem.rsplit('/',1)[-1] in HEROES

def repair(source, original, output):
    if source.resolve()==output.resolve():raise ValueError('Choose a separate output file')
    raw=original.read_bytes();chunks={};p=0
    while p+8<=len(raw):
        name=raw[p:p+4][::-1].decode('ascii');size=struct.unpack_from('<I',raw,p+4)[0]
        if p+8+size>len(raw):raise ValueError('Truncated WMO')
        chunks[name]=raw[p+8:p+8+size];p+=8+size
    names=chunks['MODN'];placements=chunks['MODD'];wanted=set()
    for i in range(len(placements)//40):
        off=struct.unpack_from('<I',placements,i*40)[0]&0xffffff
        if hero(names[off:].split(b'\0')[0].decode('ascii')):wanted.add(i)
    with source.open('rb') as f:
        magic,version,count,toc=struct.unpack('<4s3I',f.read(16))
        if magic!=b'WPAK' or version!=1:raise ValueError('Not WPAK v1')
        f.seek(toc);entries=[struct.unpack('<48s4I',f.read(64)) for _ in range(count)]
        if entries[0][0].split(b'\0')[0]!=b'!wmo':raise ValueError('Missing first !wmo')
        f.seek(entries[0][1]);header=bytearray(f.read(entries[0][2]));records={}
        if header[:4]!=b'WBWO':raise ValueError('Not WBWO')
        if struct.unpack_from('<I',header,132)[0]:raise ValueError('Landmarks already present; rebuild from the original pack')
        for nm,off,size,_,_ in entries:
            if not nm.startswith(b'dd_'):continue
            if size%76:raise ValueError('Invalid doodad blob')
            f.seek(off);blob=f.read(size)
            for pos in range(0,size,76):
                index=struct.unpack_from('<I',blob,pos+8)[0]
                if index in wanted:records[index]=blob[pos:pos+76]
    if not records or set(records)!=wanted or len(records)>16:raise ValueError('Incomplete or oversized landmark list')
    landmark_offset=(len(header)+3)&~3
    header.extend(b'\0'*(landmark_offset-len(header)))
    struct.pack_into('<I',header,132,landmark_offset)
    header.extend(struct.pack('<I',len(records)))
    for idx in sorted(records):header.extend(records[idx])
    output.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,output)
    with output.open('r+b') as f:
        f.seek(0,2);off=(f.tell()+63)&~63;f.write(b'\0'*(off-f.tell()));f.write(header)
        nm,_,_,kind,reserved=entries[0];f.seek(toc);f.write(struct.pack('<48s4I',nm,off,len(header),kind,reserved))
    # Names, TOC order/count and fence indices did not change, nor any geometry references.
    print(f'{len(records)} landmark placements, {len({struct.unpack_from("<H",b)[0] for b in records.values()})} models -> {output}')
    return records

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--pak',required=True,type=Path);p.add_argument('--wmo',required=True,type=Path);p.add_argument('--out',required=True,type=Path);a=p.parse_args();repair(a.pak,a.wmo,a.out)
