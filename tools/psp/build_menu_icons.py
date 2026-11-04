#!/usr/bin/env python3
"""Bake the player's existing WoW menu art into optional data/menuicons.pak."""
import argparse,struct
from pathlib import Path
from PIL import Image
ICONS=[
 ('settings','buttons/ui-microbutton-mainmenu-up'),
 ('character','icons/inv_helmet_03'),
 ('talents','buttons/ui-microbutton-talents-up'),
 ('quests','buttons/ui-microbutton-quest-up'),
 ('bags','icons/inv_misc_bag_08'),
 ('map','buttons/ui-microbutton-world-up'),
 ('tutorials','buttons/ui-microbutton-help-up'),
 ('social','buttons/ui-microbutton-socials-up'),
 ('achievements','buttons/ui-microbutton-achievement-up'),
 ('dungeons','buttons/ui-microbutton-lfg-up'),
 ('logout','buttons/ui-spellbookicon-prevpage-up'),
 ('exit','buttons/ui-panel-minimizebutton-up'),
 ('book','buttons/ui-microbutton-spellbook-up'),
 ('back','buttons/ui-spellbookicon-nextpage-up'),
]
def texture(image):
 image=image.convert('RGBA');box=image.getbbox()
 if box:image=image.crop(box)
 image.thumbnail((28,28),Image.Resampling.LANCZOS)
 canvas=Image.new('RGBA',(32,32));canvas.alpha_composite(image,((32-image.width)//2,(32-image.height)//2))
 pixels=b''.join(struct.pack('<H',(r>>4)|((g>>4)<<4)|((b>>4)<<8)|((a>>4)<<12)) for r,g,b,a in struct.iter_unpack('4B',canvas.tobytes()))
 # GE swizzle: 16-byte by 8-row blocks, byte stride 64.
 swizzled=b''.join(pixels[(y+j)*64+x:(y+j)*64+x+16] for y in range(0,32,8) for x in range(0,64,16) for j in range(8))
 header=struct.pack('<4s4B6H4B2I',b'WTEX',2,1,5,0,32,32,32,32,0,0,0,0,0,0,0,32)
 return header+swizzled

def build(extracted,out):
 root=Path(extracted);entries=[]
 for name,relative in ICONS:
  paths=[root/'interface'/(relative+ext) for ext in ('.png','.blp')]
  path=next((p for p in paths if p.is_file()),None)
  if path is None:raise FileNotFoundError(paths[-1])
  with Image.open(path) as image:entries.append((name,texture(image)))
 offset=16+64*len(entries);toc=bytearray();data=bytearray()
 for name,blob in entries:
  pad=(-offset)%16;data.extend(bytes(pad));offset+=pad
  toc.extend(struct.pack('<48s4I',name.encode(),offset,len(blob),1,0));data.extend(blob);offset+=len(blob)
 out=Path(out);out.parent.mkdir(parents=True,exist_ok=True)
 out.write_bytes(struct.pack('<4s3I',b'WPAK',1,len(entries),16)+toc+data)
 return len(entries),out.stat().st_size
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--extracted',required=True);parser.add_argument('--out',required=True);args=parser.parse_args()
 count,size=build(args.extracted,args.out);print(f'{args.out}: {count} menu icons, {size} bytes')
