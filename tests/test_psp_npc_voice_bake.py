"""Exercise the real sound converter with WotLK-layout NPC voice fixtures."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import wave


def dbc(path, rows, strings=b"\0"):
    fields=len(rows[0])
    path.write_bytes(b"WDBC"+struct.pack("<4I",len(rows),fields,fields*4,len(strings))+
                     b"".join(struct.pack("<"+"I"*fields,*row) for row in rows)+strings)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("converter",type=Path)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="wowee-npc-voice-") as temp:
        root=Path(temp);db=root/"dbfilesclient";db.mkdir()
        # Field 2 is the creature's combat sound set; field 12 is its NPC voice personality.
        display=[0]*13;display[0]=701;display[2]=999;display[12]=33
        other=display.copy();other[0]=702;other[12]=0
        dbc(db/"creaturedisplayinfo.dbc",[display,other])
        dbc(db/"npcsounds.dbc",[[33,9101,9102,0,0]])
        strings=bytearray(b"\0")
        def text(s):
            offset=len(strings);strings.extend(s.encode()+b"\0");return offset
        rows=[]
        for id,file in [(9101,"greeting.wav"),(9102,"farewell.wav")]:
            row=[0]*29;row[0]=id;row[2]=text("TestVoice"+str(id))
            row[3]=text(file);row[23]=text("Sound/Creature/FixtureNPC");rows.append(row)
        dbc(db/"soundentries.dbc",rows,strings)
        sound=root/"sound/creature/fixturenpc";sound.mkdir(parents=True)
        with wave.open(str(sound/"greeting.wav"),"wb") as wav:
            wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(22050)
            wav.writeframes(struct.pack("<h",1200)*2205)
        out=root/"sounds.pak"
        result=subprocess.run([str(args.converter.resolve()),"--extracted",str(root),"--out",str(out),
                               "--list-missing","--verify"],capture_output=True,text=True,check=True)
        assert "missing sound/creature/fixturenpc/farewell.wav" in result.stdout
        data=out.read_bytes();magic,version,count,toc=struct.unpack_from("<4s3I",data)
        assert magic==b"WPAK" and version==1
        entries={}
        for i in range(count):
            name,offset,size,kind,reserved=struct.unpack_from("<48s4I",data,toc+i*64)
            entries[name.split(b"\0",1)[0].decode()]=data[offset:offset+size]
        assert entries["npcvoices"]==struct.pack("<5I",701,9101,9102,0,0)
        assert "errors 0" in result.stdout
    print("NPC voice bake passed: display personality, greeting/farewell IDs, missing-clip extraction and pak verification")


if __name__=="__main__":
    main()
