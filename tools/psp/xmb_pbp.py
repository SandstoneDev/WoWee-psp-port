"""Inspect, extract or replace embedded XMB art without changing the executable."""
from pathlib import Path
import argparse,struct
NAMES=("PARAM.SFO","ICON0.PNG","ICON1.PMF","PIC0.PNG","PIC1.PNG","SND0.AT3","DATA.PSP","DATA.PSAR")
def read_pbp(data):
    if len(data)<40 or data[:4]!=b"\0PBP": raise ValueError("invalid PBP header")
    version=struct.unpack_from("<I",data,4)[0]
    offsets=list(struct.unpack_from("<8I",data,8))+[len(data)]
    if offsets[0]!=40 or offsets!=sorted(offsets) or offsets[-2]>len(data): raise ValueError("invalid PBP offsets")
    return version,{name:data[offsets[i]:offsets[i+1]] for i,name in enumerate(NAMES)}
def write_pbp(version,sections):
    chunks=[sections.get(name,b"") for name in NAMES];at=40;offsets=[]
    for data in chunks: offsets.append(at);at+=len(data)
    return b"\0PBP"+struct.pack("<I8I",version,*offsets)+b"".join(chunks)
def check_png(data,size):
    if len(data)<24 or data[:8]!=b"\x89PNG\r\n\x1a\n" or struct.unpack_from(">II",data,16)!=size: raise ValueError("XMB PNG has invalid format or dimensions")
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode",choices=["inspect","extract","repack"])
    parser.add_argument("eboot",type=Path);parser.add_argument("art",type=Path,nargs="?")
    args=parser.parse_args();version,sections=read_pbp(args.eboot.read_bytes())
    if args.mode=="inspect":
        for name in NAMES: print(f"{name}: {len(sections[name])} bytes")
        return 0
    if args.art is None: parser.error("art directory is required")
    if args.mode=="extract":
        if not sections["ICON0.PNG"] or not sections["PIC1.PNG"]: return 2
        args.art.mkdir(parents=True,exist_ok=True)
        for name in ["ICON0.PNG","PIC1.PNG"]: (args.art/name).write_bytes(sections[name])
    else:
        for name,size in [("ICON0.PNG",(144,80)),("PIC1.PNG",(480,272))]:
            data=(args.art/name).read_bytes();check_png(data,size);sections[name]=data
        temp=args.eboot.with_suffix(".PBP.tmp")
        temp.write_bytes(write_pbp(version,sections));temp.replace(args.eboot)
    return 0
if __name__=="__main__": raise SystemExit(main())
