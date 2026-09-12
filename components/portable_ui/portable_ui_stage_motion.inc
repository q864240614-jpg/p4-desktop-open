/* Shared full-card particles: stage changes motion, never draws machinery. */
#define STAGE_PERIOD 9200
typedef struct { PrinterEffect effect; int stage,phase,sway,settle; uint32_t time; } StagePose;
typedef struct { int x,y,light; } StageParticle;
static int stage_ease(int elapsed,int duration)
{
    int q=LV_CLAMP(0,elapsed,duration)*256/duration;
    return q*q*(768-2*q)/65536;
}
static uint32_t particle_hash(uint32_t seed)
{
    seed^=seed>>16;seed*=0x7feb352d;seed^=seed>>15;seed*=0x846ca68b;
    return seed^(seed>>16);
}
static StagePose stage_pose(int stage,PrinterEffect effect,uint32_t time)
{
    return (StagePose){effect,stage,(int)(time%STAGE_PERIOD),
        printer_sine(time,6200)*256/32767,
        (32767-printer_sine(time+STAGE_PERIOD/4,STAGE_PERIOD))*256/65534,time};
}
static int stage_rim(int x,int y)
{
    int edge=LV_MIN(LV_MIN(x,PARTICLE_WIDTH-1-x),LV_MIN(y,PARTICLE_HEIGHT-1-y));
    int rail=LV_MAX(0,12-abs(edge-22));
    int grid=((x%24)<2 || (y%24)<2)?10:0;
    return rail*3+grid;
}
static StageParticle stage_particle(const StagePose *p,int x,int y)
{
    int wave=printer_sine(p->time+y*17+x*9,7200);
    StageParticle dot={x+wave*2/32767,y+p->sway/128,48+(wave+32767)*16/65534};
    int lane=y/48,interior=LV_MIN(LV_MIN(x,PARTICLE_WIDTH-1-x),LV_MIN(y,PARTICLE_HEIGHT-1-y));
    switch(p->effect){
    case FX_STILL:dot.x=x;dot.y=y;dot.light=34;break;
    case FX_WATER:dot.light=42+(wave+32767)*14/65534;break;
    case FX_HEAT:{
        int rise=(y+(int)(p->time/32)+x/7)%64;
        dot.y=y-(int)(p->time/80%5);dot.light+=LV_MAX(0,20-abs(rise-32))*4;break;
    }
    case FX_SCAN:{
        int head=-32+p->phase*320/STAGE_PERIOD,distance=head-x;
        dot.light+=LV_MAX(0,16-abs(distance))*7;
        if(distance>=0 && distance<64)dot.light+=(64-distance)/2;
        dot.x+=LV_MAX(0,18-abs(distance))/6;break;
    }
    case FX_LEVEL:{
        const int offsets[]={-14,11,-8};dot.y+=offsets[x*3/PARTICLE_WIDTH]*(256-p->settle)/256;
        dot.light+=LV_MAX(0,7-abs(y%48-24))*13;break;
    }
    case FX_FEED:{
        int travel=(int)(p->time*48/1000);
        int top=PARTICLE_HEIGHT*96/300+printer_sine(p->time+x*19,3400)*6/32767;
        int bot=PARTICLE_HEIGHT*204/300+printer_sine(p->time+x*19+1700,3400)*6/32767;
        int on_top=LV_MAX(0,30-abs(y-top)),on_bot=LV_MAX(0,30-abs(y-bot));
        int out_bead=LV_MAX(0,22-abs(((x+travel)%56+56)%56-28));
        int in_bead=LV_MAX(0,22-abs(((x-travel)%56+56)%56-28));
        int top_dir,bot_dir,top_bead,bot_bead;
        if(p->stage==22){top_dir=bot_dir=-1;top_bead=bot_bead=out_bead;}
        else if(p->stage==24){top_dir=bot_dir=1;top_bead=bot_bead=in_bead;}
        else {top_dir=-1;bot_dir=1;top_bead=out_bead;bot_bead=in_bead;}
        int add=on_top*(8+top_bead)/6+on_bot*(8+bot_bead)/6;
        if(p->stage==4){
            int column=LV_MAX(0,18-abs(x-PARTICLE_CX))*LV_MAX(0,36-abs(y-PARTICLE_CY))/18;
            int handoff=LV_MAX(0,16-abs(((y+travel)%40)-20));
            add+=column*(10+handoff)/8;
        }
        dot.light+=add;
        int pull=on_top*top_dir+on_bot*bot_dir;
        dot.x+=pull*6/30;
        int cy=on_top>=on_bot?top:bot,on=LV_MAX(on_top,on_bot);
        dot.y+=(cy-y)*on/70;break;
    }
    case FX_TOOL:{
        int shift=(int)(p->time*48/1000);
        int dist=((x+lane*13-shift)%72+72)%72;
        dot.light+=LV_MAX(0,14-abs(dist-36))*5;
        dot.x+=printer_sine(p->time+y*11,2400)*5/32767;
        dot.y+=printer_sine(p->time+x*7,3100)*2/32767;break;
    }
    case FX_FLOW:{
        int travel=(int)(p->time*36/1000)+lane*20;
        int dist=((x-travel)%84+84)%84;
        dot.light+=LV_MAX(0,16-abs(dist-42))*4;
        dot.x+=printer_sine(p->time+y*19,2600)*6/32767;
        dot.y+=printer_sine(p->time+x*13,3400)*3/32767;break;
    }
    case FX_CLEAN:{
        int head=PARTICLE_CX+p->sway/4;dot.x+=p->sway/40;
        dot.light+=LV_MAX(0,24-abs(x-head-lane*7))*4;break;
    }
    case FX_HOME:{
        /* Outer lattice stays put; only the interior homes to origin along axes. */
        dot.light+=stage_rim(x,y);
        int tick=(int)(p->time*42/1000)%120;
        if(interior>26){
            dot.x+=(PARTICLE_CX-x)*p->settle/1100;dot.y+=(PARTICLE_CY-y)*p->settle/1200;
            int on_h=abs(y-PARTICLE_CY)<10 && abs(abs(x-PARTICLE_CX)-tick)<7;
            int on_v=abs(x-PARTICLE_CX)<10 && abs(abs(y-PARTICLE_CY)-tick)<7;
            if(on_h||on_v)dot.light+=36;
        }
        dot.light+=LV_MAX(0,12-abs(x-PARTICLE_CX))+LV_MAX(0,12-abs(y-PARTICLE_CY));
        break;
    }
    case FX_CENTER:{
        /* Zoom in and out; rim never collapses. */
        int pulse=printer_sine(p->time,3400)+32767;
        int zoom=200+pulse*120/65534;
        dot.light+=stage_rim(x,y);
        if(interior>24){dot.x=PARTICLE_CX+(x-PARTICLE_CX)*zoom/256;dot.y=PARTICLE_CY+(y-PARTICLE_CY)*zoom/256;}
        else {dot.x+=printer_sine(p->time+x,5200)*4/32767;dot.y+=printer_sine(p->time+y,4800)*3/32767;}
        int diamond=abs(x-PARTICLE_CX)+abs(y-PARTICLE_CY);
        int ring=36+pulse*48/65534;
        dot.light+=LV_MAX(0,12-abs(diamond-ring))*7;
        break;
    }
    case FX_OFFSET:{
        int sep=18+(256-p->settle)*40/256;
        int left=abs(x-(PARTICLE_CX-sep))+abs(y-PARTICLE_CY),right=abs(x-(PARTICLE_CX+sep))+abs(y-PARTICLE_CY);
        dot.light+=stage_rim(x,y)/2+LV_MAX(0,10-LV_MIN(left,right))*6;
        if(x<PARTICLE_CX)dot.x+=sep/6;else dot.x-=sep/6;
        break;
    }
    case FX_LIDAR:{
        int r=abs(x-PARTICLE_CX)+abs(y-PARTICLE_CY)*4/5;
        int ping=(int)(p->time*64/1000)%96;
        int hit=LV_MAX(0,9-abs(r-ping))+LV_MAX(0,7-abs(r-(ping+32)%96))+LV_MAX(0,6-abs(r-(ping+64)%96));
        int sweep=p->phase*255/STAGE_PERIOD;
        int arm=abs((x-PARTICLE_CX)*(sweep-PARTICLE_CX)/32-(y-PARTICLE_CY)*4);
        dot.light+=stage_rim(x,y)+hit*7+(r<110?LV_MAX(0,11-arm)*3:0);
        break;
    }
    case FX_CAMERA:{
        int pulse=printer_sine(p->time,3000)+32767;
        int box=48+pulse*36/65534;
        int dx=abs(x-PARTICLE_CX),dy=abs(y-PARTICLE_CY);
        int on_box=(abs(dx-box)<6 && dy<=box+4)||(abs(dy-box)<6 && dx<=box+4);
        dot.light+=stage_rim(x,y)+(on_box?32:0)+LV_MAX(0,8-abs(dx)+8-abs(dy))/2;
        if(interior>28 && on_box){dot.x+=(PARTICLE_CX-x)/18;dot.y+=(PARTICLE_CY-y)/18;}
        break;
    }
    case FX_CALIBRATE:{
        int a=80+printer_sine(p->time,2600)*24/32767,b=80-printer_sine(p->time,2600)*24/32767;
        int left=LV_MAX(0,10-abs(x-PARTICLE_CX/2))*LV_MAX(0,14-abs(y-PARTICLE_CY));
        int right=LV_MAX(0,10-abs(x-PARTICLE_CX*3/2))*LV_MAX(0,14-abs(y-PARTICLE_CY));
        dot.light+=left*a/80+right*b/80;
        break;
    }
    case FX_VIBRATION:case FX_MOTOR:{
        int vibration=printer_sine(p->time+y*3,800);
        dot.x+=vibration*(320-p->settle)/1400000;
        dot.light+=LV_MAX(0,10-abs(y%40-20))*5;break;
    }
    case FX_MOTION:
        dot.x+=p->sway/42;dot.y+=wave*3/32767;
        dot.light+=LV_MAX(0,14-abs((x+y+(int)(p->time/45))%72-36))*5;break;
    case FX_COOL:{
        int spread=stage_ease(p->time,8000);
        dot.x+=(x-PARTICLE_CX)*spread/2400;dot.y+=(y-PARTICLE_CY)*spread/2400;
        dot.light=54-spread/14;break;
    }
    case FX_IDLE:case FX_FINISH:{
        int breath=printer_sine(p->time,4200)+32767;
        int zoom=250+breath*12/65534;
        dot.x=PARTICLE_CX+(x-PARTICLE_CX)*zoom/256;
        dot.y=PARTICLE_CY+(y-PARTICLE_CY)*zoom/256;
        dot.light=36+breath*32/65534+stage_rim(x,y)/2;
        break;
    }
    case FX_VENT:{
        int rise=(int)(p->time*32/1000);
        int swirl=printer_sine(p->time+y*11,2000);
        dot.y-=(rise+y/6)%48;
        dot.x+=swirl*(28+y/10)/32767;
        int slot=LV_MAX(0,16-abs((x%52)-26))*LV_MAX(0,22-y)/22;
        int streak=LV_MAX(0,12-abs((x*2+rise)%36-18));
        dot.light+=slot*6+streak*4+(y<70?10:0);
        break;
    }
    case FX_PREP:{
        int travel=(int)(p->time*42/1000);
        const int pitch=52,h=34,mid=h/2,span=48;
        int local=((y-travel)%pitch+pitch)%pitch;
        int arm=abs(local-mid);
        int add=0;
        if(arm<=mid-2){
            int tip=mid-4;
            for(int k=0;k<3;k++){
                int xe=PARTICLE_CX-span+k*span+(tip-arm);
                add+=LV_MAX(0,7-abs(x-xe));
            }
        }
        dot.light+=add*5+LV_MAX(0,10-abs((x+y+(int)(p->time/90))%80-40));
        dot.y+=add/5;
        break;
    }
    case FX_PAUSE:dot.x=x;dot.y=y;dot.light=34+(wave+32767)*12/65534;break;
    case FX_ERROR:dot.x=x;dot.y=y;dot.light=30+(wave+32767)*16/65534;break;
    }
    dot.light=LV_MAX(10,dot.light*138/256);
    return dot;
}
