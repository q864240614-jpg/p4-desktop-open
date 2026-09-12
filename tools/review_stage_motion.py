"""Review actual LVGL RGB565 captures, not a separately reimplemented animation."""
from pathlib import Path
import json
import numpy as np
from PIL import Image, ImageDraw
root=Path(__file__).resolve().parents[1]
p=root/'assets/preview-review'
names={1:'BED LEVELING',2:'BED HEATING',3:'VIBRATION CAL',4:'FILAMENT SWAP',8:'FLOW CAL',9:'BED SCANNING',12:'LIDAR CAL',13:'HOMING',14:'NOZZLE WIPE',19:'FLOW RATIO',22:'UNLOADING',24:'LOADING',25:'MOTOR CAL',29:'COOLING',36:'MOTION CAL',39:'OFFSET CAL',46:'CAMERA CAL',62:'HOTEND TEST',70:'CENTERING'}
arrays=[];report=[]
sheet=Image.new('RGB',(272*6,338*len(names)),(8,10,9));draw=ImageDraw.Draw(sheet)
for row,(stage,name) in enumerate(names.items()):
    a=np.frombuffer((p/f'motion-{stage:02}.rgb').read_bytes(),dtype=np.uint8).reshape(-1,316,272,3)
    arrays.append(a)
    for col,index in enumerate(np.linspace(0,len(a)-1,6,dtype=int)):
        sheet.paste(Image.fromarray(a[index]),(col*272,row*338+22));draw.text((col*272+5,row*338+4),f'{name} {index*50}ms',fill='white')
    delta=np.abs(np.diff(a.astype(np.int16),axis=0)).mean(axis=(1,2,3))
    seam=float(np.abs(a[0].astype(int)-a[-1]).mean())
    # Ambient particles have their own longer cycle; their endpoints need not match.
    assert delta.max()>0,stage
    report.append(dict(stage=stage,name=name,ambient_endpoint_difference=seam,max_frame_mean=float(delta.max()),max_at_ms=int(delta.argmax()*50)))
sheet.save(p/'motion-final-phases.png')
frames=[]
for index in range(len(arrays[0])-1):
    image=Image.new('RGB',(1088,900),(4,6,5));d=ImageDraw.Draw(image)
    for slot,((stage,name),a) in enumerate(zip(names.items(),arrays)):
        x=slot%4*272;y=slot//4*180
        d.text((x+8,y+6),name,fill=(160,171,164))
        image.paste(Image.fromarray(a[index]).resize((256,148)),(x+8,y+24))
    d.text((824,870),f'{index*50/1000:.2f} s / {(len(arrays[0])-1)*.05:.2f} s',fill='white')
    frames.append(image)
frames[0].save(p/'motion-review.gif',save_all=True,append_images=frames[1:],duration=50,loop=0,optimize=False)
(p/'motion-review.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
