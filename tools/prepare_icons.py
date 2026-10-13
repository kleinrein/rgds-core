"""Extract generated icon cells and prepare bounded alpha BMPs for native SDL."""
from pathlib import Path
import struct
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]/'assets/icons'
for source,columns,rows,names in [
 ('navigation-source.png',2,2,['settings','games','favorites','recent']),
 ('settings-source.png',4,2,['display','power','library','sound','wifi','system','controls','shutdown']),
 ('games-source.png',1,1,['games']),
 ('game-menu-source.png',4,2,['play','save','load','shaders','brightness','restart','home','artwork'])]:
    sheet=Image.open(ROOT/source).convert('RGBA');w,h=sheet.size
    for i,name in enumerate(names):
        image=sheet.crop((i%columns*w//columns,i//columns*h//rows,(i%columns+1)*w//columns,(i//columns+1)*h//rows))
        # Crop only empty padding; normalize optical size within a shared canvas.
        bounds=image.getchannel('A').point(lambda a:255 if a>24 else 0).getbbox()
        image=image.crop(bounds);image.thumbnail((116,116),Image.Resampling.LANCZOS)
        canvas=Image.new('RGBA',(128,128));canvas.alpha_composite(image,((128-image.width)//2,(128-image.height)//2))
        pixels=canvas.tobytes('raw','BGRA')
        dib=struct.pack('<IiiHHIIiiII',108,128,-128,1,32,3,len(pixels),0,0,0,0)
        dib+=struct.pack('<5I',0xff0000,0xff00,0xff,0xff000000,0x73524742)+bytes(48)
        (ROOT/(name+'.bmp')).write_bytes(struct.pack('<2sIHHI',b'BM',122+len(pixels),0,0,122)+dib+pixels)
