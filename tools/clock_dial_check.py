"""Check the native P4 digit geometry against generated font metrics."""
from pathlib import Path
import math
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'components/portable_ui/portable_ui.c').read_text(encoding='utf-8')

def pair(name):
    m = re.search(r'const int16_t ' + name + r' = is_hour \? (-?\d+) : (-?\d+);', SOURCE)
    if m: return tuple(map(int, m.groups()))
    constant = re.search(r'const int16_t ' + name + r' = (-?\d+);', SOURCE)
    if constant: return (int(constant[1]),) * 2
    geometry_source = SOURCE.split('clock_geometry[2] = {',1)[1].split('};',1)[0]
    values = re.findall(r'\.' + name + r' = (-?\d+)', geometry_source)
    assert len(values)==2, name
    return tuple(map(int, values))

FONTS = [(ROOT / f'components/portable_ui/lv_font_inter_{size}.c').read_text(encoding='utf-8') for size in (120, 120)]
WIDTH = pair('width')
HEIGHT = tuple(int(re.search(r'\.line_height = (\d+)', s)[1]) + 8 for s in FONTS)
DIRECTION = (1, -1)
CX, CY, RADIUS, PITCH, MARKER = [pair(name) for name in ('center_x', 'center_y', 'radius', 'pitch', 'marker_x')]
BOUNDS = []
for name in ('hour_dial', 'minute_dial'):
    x,y = map(int, re.search(r'lv_obj_set_pos\(' + name + r', (\d+), (\d+)\)', SOURCE).groups())
    w,h = map(int, re.search(r'lv_obj_set_size\(' + name + r', (\d+), (\d+)\)', SOURCE).groups())
    BOUNDS.append((x,y,x+w,y+h))

MATH = ROOT / 'managed_components/lvgl__lvgl/src/misc/lv_math.c'
if not MATH.is_file():
    sys.exit('LVGL lv_math.c is not in this tree yet; run idf.py once to fetch managed_components, or skip this check')
TRIG_SOURCE=MATH.read_text(encoding='utf-8')
SINES=list(map(int,re.findall(r'\d+',TRIG_SOURCE.split('sin0_90_table[] = {')[1].split('};')[0])))

def sine(degree):
    degree%=360
    if degree<=90:return SINES[degree]
    if degree<=180:return SINES[180-degree]
    if degree<=270:return -SINES[degree-180]
    return -SINES[360-degree]

def native_mask_bounds(i,phase,offset):
    # Match the bundled LVGL integer trig, transformed-area AA border and +1px shift.
    angle_q8=math.trunc((phase-2048)*PITCH[i]/16)-offset*PITCH[i]*256
    rotation=angle_q8%(3600*256);degree,fraction=divmod(rotation,2560)
    c=math.trunc((sine(degree+90)*(2560-fraction)+sine(degree+91)*fraction)/2560)
    t=math.trunc((sine(degree)*(2560-fraction)+sine(degree+1)*fraction)/2560)
    x=(CX[i]*256+DIRECTION[i]*(RADIUS[i]*c>>7))>>8
    y=(CY[i]*256+(RADIUS[i]*t>>7))>>8
    angle=(DIRECTION[i]*math.trunc(angle_q8/256))%3600
    w,h=WIDTH[i],HEIGHT[i]
    if angle==0:return x-w//2,y-h//2,x-w//2+w,y-h//2+h
    low,rem=divmod(angle,10)
    sinma=math.trunc((sine(low)*(10-rem)+sine(low+1)*rem)/10)>>5
    cosma=math.trunc((sine(low+90)*(10-rem)+sine(low+91)*rem)/10)>>5
    points=[(x+((cosma*dx-sinma*dy)>>10),y+((sinma*dx+cosma*dy)>>10))
            for dx,dy in ((-w//2,-h//2),(w//2,-h//2),(-w//2,h//2),(w//2,h//2))]
    l,t,r,b=bbox(points)
    return l-2,t-2,r+3,b+3

def geometry(i, phase, offset):
    angle = math.trunc((phase - 2048) * PITCH[i] / 4096) - offset * PITCH[i]
    a = math.radians(angle / 10)
    c,s = math.cos(a),math.sin(a)
    x,y = CX[i] + math.floor(DIRECTION[i]*RADIUS[i]*c), CY[i] + math.floor(RADIUS[i]*s)
    angle *= DIRECTION[i]
    s *= DIRECTION[i]
    corners = [(x+dx*c-dy*s,y+dx*s+dy*c) for dx,dy in (
        (-WIDTH[i]/2,-HEIGHT[i]/2),(WIDTH[i]/2,-HEIGHT[i]/2),
        (WIDTH[i]/2,HEIGHT[i]/2),(-WIDTH[i]/2,HEIGHT[i]/2))]
    return angle,x,y,corners

def bbox(corners):
    return min(p[0] for p in corners),min(p[1] for p in corners),max(p[0] for p in corners),max(p[1] for p in corners)

def intersects(a,b):
    for poly in (a,b):
        for p,q in zip(poly,poly[1:]+poly[:1]):
            axis=(p[1]-q[1],q[0]-p[0])
            aa=[x*axis[0]+y*axis[1] for x,y in a]
            bb=[x*axis[0]+y*axis[1] for x,y in b]
            if max(aa)<=min(bb) or max(bb)<=min(aa): return False
    return True

def safe_margin(i,phase):
    angle_q8=math.trunc((phase-524288)*PITCH[i]/4096)
    rotation=angle_q8%(3600*256);degree,fraction=divmod(rotation,2560)
    c=math.trunc((sine(degree+90)*(2560-fraction)+sine(degree+91)*fraction)/2560)
    t=math.trunc((sine(degree)*(2560-fraction)+sine(degree+1)*fraction)/2560)
    x=CX[i]*256+DIRECTION[i]*(RADIUS[i]*c>>7);y=CY[i]*256+(RADIUS[i]*t>>7)
    dx=abs(x-CX[i]*256);dy=abs(y-CY[i]*256)
    hw=(WIDTH[i]*dx+HEIGHT[i]*dy)//(2*RADIUS[i]);hh=(WIDTH[i]*dy+HEIGHT[i]*dx)//(2*RADIUS[i])
    return min(y-108*256,420*256-y)

VISIBLE=[]
for i in range(2):
    ends=[]
    for end in (0,1):
        lo,hi=(524288,2621440) if end else (-1572864,524288)
        while hi-lo>1:
            mid=lo+(hi-lo)//2
            if (safe_margin(i,mid)>=0)==(end==0):hi=mid
            else:lo=mid
        ends.append(lo if end else hi)
    VISIBLE.append(ends)

def ease(ms):
    t=max(0,min(ms,800))/800
    return t*t*(3-2*t)

def opacity(i,phase,offset,angle,box):
    period=3600000 if i==0 else 60000
    age=phase*period/4096-offset*period
    enter,leave=[p*period/1048576 for p in VISIBLE[i]]
    return (40+215*ease(min(age,period-age)))*ease(min(age-enter,leave-age))


def progress_ms(i,phase): return phase*(3600000 if i==0 else 60000)/4096

def check():
    swept=[[],[]]
    lower_clipped=[False,False]
    for phase in range(4097):
        for i in range(2):
            shapes=[]
            for offset in range(-2,3):
                angle,x,y,corners=geometry(i,phase,offset)
                box=bbox(corners); alpha=opacity(i,phase,offset,angle,box)
                if offset==0:
                    x0,y0,x1,y1=BOUNDS[i]
                    assert box[0]>=x0 and box[1]>=y0 and box[2]<=x1 and box[3]<=y1, ('clipped current',i,phase,box)
                    l,t,r,b=native_mask_bounds(i,phase,offset)
                    assert l>=x0 and t>=y0 and r<x1 and b<y1, ('native mask clipped',i,phase,(l,t,r,b))
                    assert alpha>0, ('current disappeared',i,phase)
                elif 400<=progress_ms(i,phase)<=((3600000 if i==0 else 60000)-400): assert alpha<=40.001, ('neighbor highlighted',i,phase)
                if alpha>0:
                    x0,y0,x1,y1=BOUNDS[i]
                    assert box[2]>x0 and box[3]>y0 and box[0]<x1 and box[1]<y1, ('invisible neighbor',i,phase,offset)
                    if offset!=0 and y>=400 and box[3]>BOUNDS[i][3]:lower_clipped[i]=True
                    shapes.append(corners)
                    x0,y0,x1,y1=BOUNDS[i]
                    swept[i].append((max(box[0],x0),max(box[1],y0),min(box[2],x1),min(box[3],y1)))
                    for j in range(2):
                        if CY[j]+3 < BOUNDS[i][1] or CY[j]-3 >= BOUNDS[i][3]: continue
                        ml = MARKER[j] - (3 if j == 0 else 23)
                        mr = MARKER[j] + (23 if j == 0 else 3)
                        marker=[(ml,CY[j]-3),(mr,CY[j]-3),(mr,CY[j]+3),(ml,CY[j]+3)]
                        assert not intersects(corners,marker), ('marker collision',i,phase,offset,j)
            assert 2 <= len(shapes) <= 3, ("visible digit count",i,phase,len(shapes))
            for a,b in zip(shapes,shapes[1:]):
                assert not intersects(a,b), ('digits overlap',i,phase)
    for i,period in enumerate((3600000,60000)):
        start=(1+VISIBLE[i][0]/1048576)*period
        assert .48*period<start<.52*period, "next value should enter near the second half"
    assert all(lower_clipped), "lower neighbors must remain visible while clipped"
    # Full sweep envelopes must leave a clear gap between the two digit columns.
    assert max(b[2] for b in swept[0])+2 < min(b[0] for b in swept[1]), 'dial columns overlap'
    for i,mod in enumerate((24,60)):
        for offset in (0,1):
            assert geometry(i,4096,offset)==geometry(i,0,offset-1), 'rollover position jump'
            assert (mod-1+offset)%mod == (offset-1+mod)%mod, 'midnight value jump'
            a,_,_,corners=geometry(i,4096,offset)
            before=opacity(i,4096,offset,a,bbox(corners))
            after=opacity(i,0,offset-1,a,bbox(corners))
            assert abs(before-after)<.01, ('rollover opacity jump',i,offset,before,after)
    assert SOURCE.count('lv_anim_set_time(&animation, 280)')==2
    assert 'set_standby_opacity' not in SOURCE
    assert 'lv_obj_set_x(control_layer, x - 800)' in SOURCE
    assert 'progress * 1048576 / period' in SOURCE
    # Sample each entry and exit at 8 ms, including the much slower hour wheel.
    for i,period in enumerate((3600000,60000)):
        enter,leave=[p*period/1048576 for p in VISIBLE[i]]
        entering=[40*ease(ms) for ms in range(0,801,8)]
        assert entering[0]==0 and entering[-1]==40
        assert len(set(entering))==101
        assert all(a<b for a,b in zip(entering,entering[1:]))
        for age in (enter,enter+800,leave-800,leave):
            phase=age*4096/period
            a,_,_,corners=geometry(i,phase,0)
            value=opacity(i,phase,0,a,bbox(corners))
            assert abs(value-(0 if age in (enter,leave) else 40))<.01
    # The old digit is already dim when the new digit starts rising.
    for period in (60000,3600000):
        def light(age):
            t=max(0,min(800,age,period-age))/800
            return 40+215*t*t*(3-2*t)
        for ms in range(-800,801,8):
            old,new=light(period+ms),light(ms)
            assert old==40 or new==40
            if ms<0:assert new==40
            if ms>0:assert old==40
        assert light(period-800)==255 and light(period)==40
        assert light(0)==40 and light(800)==255
    print('PASS: 4097 poses, 2–3 visible digits including clipped neighbors, no overlaps, rollover continuity, sequential 800 ms handoff')

if __name__ == '__main__':
    check()
