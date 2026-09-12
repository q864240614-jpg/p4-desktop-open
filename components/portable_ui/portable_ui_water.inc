/* Alpha field matches the 272x316 printer card, including the former 8px inset. */
#define PARTICLE_WIDTH 272
#define PARTICLE_HEIGHT 316
#define PARTICLE_CX (PARTICLE_WIDTH/2)
#define PARTICLE_CY (PARTICLE_HEIGHT/2)
#define CARD_RADIUS 16
#include "portable_ui_stage_motion.inc"
/* Two to four randomly placed arrivals per window, independently seeded each boot. */
typedef struct { int x,age,fall,size,count,spread; uint32_t seed; } WaterCluster;
static WaterCluster water_cluster(uint32_t seed,uint32_t window,int index,uint32_t time,int surface,int speed,uint32_t span)
{
    uint32_t random=particle_hash(seed+window*7919u+index*104729u);
    uint32_t born=window*span+random%span;
    int fall=(200+surface*(2+(int)((random>>20)%3)))*(6-speed)/3;
    return (WaterCluster){.x=16+(random>>12)%(PARTICLE_WIDTH-32),.age=(int32_t)(time-born),
        .fall=LV_MAX(180,fall),.size=3+(random>>24)%4,
        .count=2+speed+(int)((random>>18)%(speed+1)),.spread=16+speed*4+(random>>7)%24,.seed=random};
}
static void particle_glow(uint8_t *pixels,int cx,int cy,int radius,int light)
{
    for(int y=LV_MAX(0,cy-radius);y<=LV_MIN(PARTICLE_HEIGHT-1,cy+radius);y++)
        for(int x=LV_MAX(0,cx-radius);x<=LV_MIN(PARTICLE_WIDTH-1,cx+radius);x++){
            int distance=abs(x-cx)+abs(y-cy);
            int add=LV_MAX(0,radius+1-distance)*light/(radius+1);
            pixels[y*PARTICLE_WIDTH+x]=LV_MIN(255,pixels[y*PARTICLE_WIDTH+x]+add);
        }
}
static int particle_cover(int x,int y)
{
    const int r=CARD_RADIUS,fade=4;
    int dx=0,dy=0;
    if(x<r)dx=r-x;
    else if(x>PARTICLE_WIDTH-1-r)dx=x-(PARTICLE_WIDTH-1-r);
    if(y<r)dy=r-y;
    else if(y>PARTICLE_HEIGHT-1-r)dy=y-(PARTICLE_HEIGHT-1-r);
    if(dx && dy){
        int d2=dx*dx+dy*dy,outer=r*r,inner=(r-fade)*(r-fade);
        if(d2>=outer)return 0;
        if(d2<=inner)return 256;
        return (outer-d2)*256/(outer-inner);
    }
    int edge=LV_MIN(LV_MIN(x,PARTICLE_WIDTH-1-x),LV_MIN(y,PARTICLE_HEIGHT-1-y));
    return edge>=fade?256:(edge+1)*256/(fade+1);
}
static void particle_soften(PrinterPage *p)
{
    uint8_t *pixels=(uint8_t *)p->water.data,*blur=p->particle_blur;
    /* Separable radius-2 box blur, mixed into the field underneath information. */
    for(int y=0;y<PARTICLE_HEIGHT;y++){
        int sum=0;
        for(int x=0;x<2;x++)sum+=pixels[y*PARTICLE_WIDTH+x];
        for(int x=0;x<PARTICLE_WIDTH;x++){
            if(x+2<PARTICLE_WIDTH)sum+=pixels[y*PARTICLE_WIDTH+x+2];
            if(x>2)sum-=pixels[y*PARTICLE_WIDTH+x-3];
            blur[y*PARTICLE_WIDTH+x]=sum/5;
        }
    }
    for(int x=0;x<PARTICLE_WIDTH;x++){
        int sum=0;
        for(int y=0;y<2;y++)sum+=blur[y*PARTICLE_WIDTH+x];
        for(int y=0;y<PARTICLE_HEIGHT;y++){
            if(y+2<PARTICLE_HEIGHT)sum+=blur[(y+2)*PARTICLE_WIDTH+x];
            if(y>2)sum-=blur[(y-3)*PARTICLE_WIDTH+x];
            int idx=y*PARTICLE_WIDTH+x,fog=p->fog_weights[idx];
            int soft=(pixels[idx]*96+(sum/5)*160)/256;
            soft=soft*210/256;
            int alpha=(pixels[idx]*(255-fog)+soft*fog)/255;
            alpha=alpha*(255-fog*5/8)/255;
            pixels[idx]=alpha*particle_cover(x,y)/256;
        }
    }
}
/* Original subpixel LED water kernel: soft core, fine halo, no solid diamond sprites. */
static void water_glow(PrinterPage *p,int cx,int cy,int rx,int ry,int strength)
{
    uint8_t *pixels=(uint8_t *)p->water.data;
    for(int y=LV_MAX(0,(cy>>8)-ry);y<=LV_MIN(PARTICLE_HEIGHT-1,(cy>>8)+ry+1);y++)
        for(int x=LV_MAX(0,(cx>>8)-rx);x<=LV_MIN(PARTICLE_WIDTH-1,(cx>>8)+rx+1);x++){
            int dx=abs(x*256-cx),dy=abs(y*256-cy);
            int coverage=LV_MAX(0,256-dx/rx-dy/ry);
            int gx=(x+(y/5%2)*2)%5,gy=y%5;
            int led=gx<2 && gy<2?255:gx<3 && gy<3?90:24;
            int add=coverage*strength/256*led/255;
            pixels[y*PARTICLE_WIDTH+x]=LV_MIN(255,pixels[y*PARTICLE_WIDTH+x]+add);
        }
}
static void render_printer_water(PrinterPage *p)
{
    p->particle_renders++;
    uint8_t *pixels=(uint8_t *)p->water.data;
    memset(pixels,0,p->water.data_size);
    PrinterEffect effect=printer_effect(&p->view);
    unsigned age=p->motion_ms-p->stage_changed;
    int blend=stage_ease(age,1200);
    StagePose pose=stage_pose(p->view.stage,effect,age);
    StagePose previous=stage_pose(p->previous_stage,p->previous_effect,p->previous_motion+age);
    for(int y=4;y<PARTICLE_HEIGHT-4;y+=5)for(int x=4;x<PARTICLE_WIDTH-4;x+=5){
        uint32_t seed=particle_hash(p->particle_seed+x*31+y*137);
        StageParticle dot=stage_particle(&pose,x,y);
        if(blend<256){
            StageParticle old=stage_particle(&previous,x,y);
            dot.x=(old.x*(256-blend)+dot.x*blend)/256;
            dot.y=(old.y*(256-blend)+dot.y*blend)/256;
            dot.light=(old.light*(256-blend)+dot.light*blend)/256;
        }
        particle_glow(pixels,dot.x,dot.y,2+(seed%7==0),dot.light*(136+(seed>>8)%40)/256);
    }
    int water_gain=effect==FX_WATER?blend:p->previous_effect==FX_WATER?256-blend:0;
    if(water_gain){
        int level=LV_MAX(0,p->view.progress),center=PARTICLE_HEIGHT-8-level*(PARTICLE_HEIGHT-24)/100;
        int speed=p->view.speed_level;if(speed<1 || speed>4)speed=2;
        uint32_t span=5200u-(unsigned)(speed-1)*900u;
        WaterCluster clusters[16];int count=0;
        uint32_t now=p->motion_ms,window=now/span;
        for(int back=0;back<3;back++){
            uint32_t block=window-back;
            int arrivals=speed+(int)(particle_hash(p->particle_seed+block*7919u)%(speed==1?1u:2u));
            for(int i=0;i<arrivals && count<16;i++)
                clusters[count++]=water_cluster(p->particle_seed,block,i,now,center,speed,span);
        }
        int surface[PARTICLE_WIDTH];
        for(int x=0;x<PARTICLE_WIDTH;x++){
            int q=center*256+(printer_sine(now+x*11,3400)*3*256+printer_sine(now+x*23,5700)*256)/32767;
            for(int i=0;i<count;i++){
                const WaterCluster *c=&clusters[i];int impact=c->age-c->fall;
                if(impact>=0 && impact<1600){
                    int d=abs(x-c->x)-impact*(65+c->spread)/1600;
                    int decay=(1600-impact)*(1600-impact)/1600;
                    q-=(LV_MAX(0,12-abs(d))-LV_MAX(0,7-abs(d-16)))*decay*c->size*256/(12*1600);
                }
            }
            surface[x]=q;
            for(int y=LV_MAX(0,(q>>8)-3);y<PARTICLE_HEIGHT;y++){
                int depth=y*256-q;
                int light=LV_MAX(0,768-abs(depth))*88/768;
                if(depth>=0)light+=6+(x%5<2 && y%5<2?14:0);
                pixels[y*PARTICLE_WIDTH+x]=LV_MIN(255,pixels[y*PARTICLE_WIDTH+x]+light*water_gain/256);
            }
        }
        for(int i=0;i<count;i++){
            const WaterCluster *c=&clusters[i];int impact=c->age-c->fall;
            int at=surface[c->x];
            if(c->age>=0 && c->age<c->fall){
                int cy=(int)((int64_t)c->age*c->age*at/(c->fall*c->fall));
                int fade=LV_MIN(256,c->age*256/180);
                for(int tail=0;tail<3;tail++){
                    int drift=printer_sine(now+i*1100+tail*330,4100)*3*256/32767;
                    water_glow(p,c->x*256+drift,cy-tail*5*256,c->size+2,c->size+2,
                        (210-tail*55)*fade/256*water_gain/256);
                }
            }else if(impact>=0 && impact<1000){
                for(int j=0;j<c->count;j++){
                    uint32_t random=particle_hash(c->seed+j*65537u);
                    int duration=480+j*39+random%90;
                    if(impact>=duration)continue;
                    int direction=2*j-(c->count-1);
                    int cx=c->x*256+direction*impact*c->spread*256/(duration*(c->count-1));
                    int height=18+(j*7+(int)(random%11))%19;
                    int cy=at-(int)((int64_t)impact*(duration-impact)*height*4*256/(duration*duration));
                    int energy=(duration-impact)*(duration-impact)*190/(duration*duration);
                    water_glow(p,cx,cy,6,5,energy*water_gain/256);
                }
            }
        }
    }
    particle_soften(p);
}
static void progress_fill_draw(lv_event_t *event)
{
    PrinterPage *p=lv_event_get_user_data(event);lv_area_t a;lv_obj_get_coords(p->fill,&a);
    lv_draw_img_dsc_t d;lv_draw_img_dsc_init(&d);
    int blend=stage_ease(p->motion_ms-p->stage_changed,1200);
    d.recolor=lv_color_mix(lv_color_hex(printer_color(&p->view)),
        lv_color_hex(printer_effect_color(p->previous_effect)),blend*255/256);
    d.recolor_opa=LV_OPA_COVER;
    lv_draw_img(lv_event_get_draw_ctx(event),&d,&a,&p->water);
}
