"""Compare the real spell converter's metadata with the supplied client DBCs."""
from pathlib import Path
import argparse,struct,subprocess,tempfile

def dbc(path):
    b=path.read_bytes();n,f,stride,_=struct.unpack_from('<4I',b,4)
    return {r[0]:r for r in (struct.unpack_from('<'+'I'*f,b,20+i*stride) for i in range(n))}

def pak(path):
    b=path.read_bytes();magic,version,n,toc=struct.unpack_from('<4s3I',b);assert magic==b'WPAK' and version==1
    out={}
    for i in range(n):
        name,offset,size,_,_=struct.unpack_from('<48s4I',b,toc+64*i)
        out[name.split(b'\0',1)[0].decode()]=b[offset:offset+size]
    return out

def main():
    parser=argparse.ArgumentParser();parser.add_argument('converter',type=Path);parser.add_argument('extracted',type=Path)
    a=parser.parse_args();db=a.extracted/'dbfilesclient'
    spells=dbc(db/'spell.dbc');visuals=dbc(db/'spellvisual.dbc');kits=dbc(db/'spellvisualkit.dbc')
    ids=[78,116,133,2366,3365,21651,22810]
    with tempfile.TemporaryDirectory(prefix='wowee-spell-metadata-') as folder:
        folder=Path(folder);wanted=folder/'ids.txt';wanted.write_text('\n'.join(map(str,ids)),encoding='utf-8')
        output=folder/'spellfx.pak'
        result=subprocess.run([str(a.converter.resolve()),'--extracted',str(a.extracted.resolve()),'--spells',str(wanted),
                               '--out',str(output),'--max-frames','4','--fps','4'],capture_output=True,text=True,check=True)
        data=pak(output);meta={r[0]:r for r in struct.iter_unpack('<8I',data['kits'])}
        starts={r[0]:r for r in struct.iter_unpack('<4I',data['kitstarts'])}
        for spell in ids:
            visual=spells[spell][131];v=visuals[visual]
            def kit(field):return kits.get(v[field])
            anim=[kit(f)[2] if kit(f) else 0xFFFFFFFF for f in (1,2,6)]
            sound=[kit(f)[15] if kit(f) else 0 for f in (1,2,3,6)]
            start=[kit(f)[1] if kit(f) else 0xFFFFFFFF for f in (1,2,6)]
            assert meta[visual]==(visual,*anim,*sound),(spell,meta[visual],anim,sound)
            assert starts[visual]==(visual,*start)
        assert meta[spells[3365][131]][1]==123 # client use-standing pose, not a generic magic cast
        assert meta[spells[21651][131]][2]==0xFFFFFFFF # no fabricated release animation
        verify=subprocess.run([str(a.converter.resolve()),'--verify',str(output)],capture_output=True,text=True,check=True)
    print('Spell metadata passed: all start/hold/release/sound fields match client rows for weapon, fire/frost and gathering cases')

if __name__=='__main__':main()
