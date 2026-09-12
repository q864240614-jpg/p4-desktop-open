#include "bambu_state.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
static bool number(const cJSON *v, int *out, int lo, int hi)
{
    double d;
    if (cJSON_IsNumber(v)) d = v->valuedouble;
    else if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end; d = strtod(v->valuestring, &end); if (*end) return false;
    } else return false;
    if (!isfinite(d) || d < lo || d > hi) return false;
    *out = (int)d; return true;
}
static void field(cJSON *p, const char *key, int *out, int hi)
{ number(cJSON_GetObjectItemCaseSensitive(p, key), out, 0, hi); }
static void string(cJSON *p, const char *key, char *out, size_t size)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    if (cJSON_IsString(v)) snprintf(out, size, "%s", v->valuestring);
}
static const char *filament_label(const char *idx, const char *sub, const char *type)
{
    if(idx && !strcmp(idx,"GFS05"))return "SUP P/G";
    if(idx && (!strcmp(idx,"GFS00") || !strcmp(idx,"GFS02")))return "SUP PLA";
    if(idx && !strcmp(idx,"GFS01"))return "SUP G";
    if(idx && !strcmp(idx,"GFS06"))return "SUP ABS";
    if(idx && (!strcmp(idx,"GFS03") || !strcmp(idx,"GFS04")))return "SUP PA";
    if(sub && strstr(sub,"PLA/PETG"))return "SUP P/G";
    if(sub && strstr(sub,"Support for PLA"))return "SUP PLA";
    if(sub && strstr(sub,"Support for ABS"))return "SUP ABS";
    if(sub && strstr(sub,"PA/PET"))return "SUP PA";
    if(sub && sub[0])return sub;
    return type && type[0]?type:"--";
}
static void parse_tray(BambuState *s, cJSON *tray, int uid, int slot)
{
    int idx=-1;
    for(int i=0;i<BAMBU_TRAYS;i++) if(s->trays[i].present && s->trays[i].unit==uid && s->trays[i].slot==slot) {idx=i;break;}
    if(idx<0) for(int i=0;i<BAMBU_TRAYS;i++) if(!s->trays[i].present){idx=i;break;}
    if(idx<0 || !tray) return;
    BambuTray *t=&s->trays[idx]; t->present=true;t->unit=uid;t->slot=slot;
    number(cJSON_GetObjectItemCaseSensitive(tray,"remain"),&t->remaining,-1,100);
    char type[16]="",sub[40]="",code[12]="";
    string(tray,"tray_type",type,sizeof type);
    string(tray,"tray_sub_brands",sub,sizeof sub);
    string(tray,"tray_info_idx",code,sizeof code);
    snprintf(t->material,sizeof t->material,"%s",filament_label(code,sub,type));
    cJSON *color=cJSON_GetObjectItemCaseSensitive(tray,"tray_color");
    if(cJSON_IsString(color) && (strlen(color->valuestring)==8 || strlen(color->valuestring)==6)) {
        char *tail; unsigned long v=strtoul(color->valuestring,&tail,16);
        if(!*tail) t->color=strlen(color->valuestring)==8 ? v>>8 : v;
    }
}
void BambuState_Init(BambuState *s)
{
    memset(s, 0, sizeof(*s));
    s->progress = s->remaining = s->layer = s->layers = s->nozzle = s->nozzle_target = -1;
    s->bed = s->bed_target = s->fan = s->active_tray = -1;
    s->active_nozzle=-1;s->stage=-1;
    s->chamber=s->speed_level=s->door_open=-1;
    for(int i=0;i<2;i++)s->nozzle_current[i]=s->nozzle_targets[i]=-1;
    for (int i=0;i<BAMBU_TRAYS;i++) s->trays[i].remaining=-1;
}
bool BambuState_Parse(BambuState *s, const char *json, size_t length)
{
    if (!s || !json || !length || length > 65536) return false;
    const char *end;
    cJSON *root=cJSON_ParseWithLengthOpts(json,length,&end,false);
    if (!root) return false;
    while (end < json+length && (*end==' ' || *end=='\r' || *end=='\n' || *end=='\t')) end++;
    cJSON *p=cJSON_GetObjectItemCaseSensitive(root,"print");
    if (end != json+length || !cJSON_IsObject(p) || !p->child) { cJSON_Delete(root); return false; }
    string(p,"gcode_state",s->state,sizeof(s->state));
    number(cJSON_GetObjectItemCaseSensitive(p,"stg_cur"),&s->stage,-1,65535);
    string(p,"subtask_name",s->name,sizeof(s->name));
    field(p,"mc_percent",&s->progress,100); field(p,"mc_remaining_time",&s->remaining,1000000);
    field(p,"layer_num",&s->layer,1000000); field(p,"total_layer_num",&s->layers,1000000);
    field(p,"nozzle_temper",&s->nozzle,500); field(p,"nozzle_target_temper",&s->nozzle_target,500);
    field(p,"bed_temper",&s->bed,200); field(p,"bed_target_temper",&s->bed_target,200);
    field(p,"chamber_temper",&s->chamber,200);
    number(cJSON_GetObjectItemCaseSensitive(p,"spd_lvl"),&s->speed_level,1,4);
    int fan=-1; field(p,"cooling_fan_speed",&fan,15); if(fan>=0) s->fan=(fan*100+7)/15;
    cJSON *err=cJSON_GetObjectItemCaseSensitive(p,"print_error");
    if(cJSON_IsNumber(err) && err->valuedouble>=0 && err->valuedouble<=UINT32_MAX) s->error=(uint32_t)err->valuedouble;
    cJSON *ams=cJSON_GetObjectItemCaseSensitive(p,"ams");
    int active=s->active_tray; if(number(cJSON_GetObjectItemCaseSensitive(ams,"tray_now"),&active,0,255)) s->active_tray=active;
    cJSON *unit;
    cJSON_ArrayForEach(unit,cJSON_GetObjectItemCaseSensitive(ams,"ams")) {
        int uid=-1; if(!number(cJSON_GetObjectItemCaseSensitive(unit,"id"),&uid,0,16)) continue;
        cJSON *tray;
        cJSON_ArrayForEach(tray,cJSON_GetObjectItemCaseSensitive(unit,"tray")) {
            int slot=-1; if(!number(cJSON_GetObjectItemCaseSensitive(tray,"id"),&slot,0,3)) continue;
            parse_tray(s,tray,uid,slot);
        }
    }
    /* H2D: vir_slot id 254 = left external, 255 = right external. vt_tray is the older single-spool form. */
    bool have_vir=false;
    cJSON *slot;
    cJSON_ArrayForEach(slot,cJSON_GetObjectItemCaseSensitive(p,"vir_slot")) {
        int id=-1; if(!number(cJSON_GetObjectItemCaseSensitive(slot,"id"),&id,254,255)) continue;
        parse_tray(s,slot,id,0); have_vir=true;
    }
    cJSON *vt=cJSON_GetObjectItemCaseSensitive(p,"vt_tray");
    if(cJSON_IsObject(vt) && !have_vir) {
        int id=254; if(!number(cJSON_GetObjectItemCaseSensitive(vt,"id"),&id,254,255)) id=254;
        parse_tray(s,vt,id,0);
    }
    cJSON *device=cJSON_GetObjectItemCaseSensitive(p,"device");
    /* H2D packed chamber temperature: target in high 16 bits, current in low 16. */
    cJSON *ctc=cJSON_GetObjectItemCaseSensitive(device,"ctc");
    cJSON *ctc_info=cJSON_GetObjectItemCaseSensitive(ctc,"info");
    int chamber_packed=-1;
    if(number(cJSON_GetObjectItemCaseSensitive(ctc_info,"temp"),&chamber_packed,0,INT32_MAX)
        && (chamber_packed&65535)<=200) s->chamber=chamber_packed&65535;
    cJSON *extruder=cJSON_GetObjectItemCaseSensitive(device,"extruder");
    if(!cJSON_IsObject(extruder))extruder=cJSON_GetObjectItemCaseSensitive(p,"extruder");
    cJSON *info=cJSON_GetObjectItemCaseSensitive(extruder,"info");
    if(cJSON_IsArray(info)) {
        if(cJSON_GetArraySize(info)>=2)s->dual_nozzle=true;
        int packed_state=-1;field(extruder,"state",&packed_state,65535);
        if(packed_state>=0 && ((packed_state>>4)&15)<=1)s->active_nozzle=(packed_state>>4)&15;
        cJSON *entry;
        cJSON_ArrayForEach(entry,info) {
            int id=-1;if(!number(cJSON_GetObjectItemCaseSensitive(entry,"id"),&id,0,1))continue;
            cJSON *temp=cJSON_GetObjectItemCaseSensitive(entry,"temp");
            if(cJSON_IsNumber(temp) && temp->valuedouble>=0 && temp->valuedouble<=UINT32_MAX) {
                uint32_t packed=(uint32_t)temp->valuedouble;
                if((packed&65535)<=500)s->nozzle_current[id]=packed&65535;
                if((packed>>16)<=500)s->nozzle_targets[id]=packed>>16;
            }
            int snow=-1;field(entry,"snow",&snow,65535);
            if(id==s->active_nozzle && snow>=0) {
                if(snow==254 || snow==255)s->active_tray=snow;
                else if(s->active_tray!=254 && s->active_tray!=255)s->active_tray=(snow>>8)*4+(snow&3);
            }
        }
        if(s->active_nozzle>=0) {
            s->nozzle=s->nozzle_current[s->active_nozzle];s->nozzle_target=s->nozzle_targets[s->active_nozzle];
        }
    }
    /* H2 stat is hexadecimal and may exceed 32 bits; A1 has no door sensor. */
    cJSON *stat=cJSON_GetObjectItemCaseSensitive(p,"stat");
    if(s->dual_nozzle && cJSON_IsString(stat)) {
        const char *hex=stat->valuestring;
        size_t digits=strlen(hex);
        if(digits>0 && digits<=16 && strspn(hex,"0123456789abcdefABCDEF")==digits)
            s->door_open=(strtoull(hex,NULL,16)&0x00800000ULL)!=0;
    }
    s->has_data=s->online=true; cJSON_Delete(root); return true;
}
void BambuFrame_Reset(BambuFrame *f)
{ free(f->data); memset(f,0,sizeof(*f)); }
bool BambuFrame_Append(BambuFrame *f, size_t total, size_t offset, const char *data, size_t size)
{
    if(!offset) {
        BambuFrame_Reset(f);
        if(total && total<=65536) { f->data=malloc(total+1);f->size=total; }
    }
    if(!f->data || !data || !size || total!=f->size || offset!=f->used || size>f->size-f->used) {
        BambuFrame_Reset(f);return false;
    }
    memcpy(f->data+f->used,data,size);f->used+=size;f->data[f->used]=0;
    return f->used==f->size;
}
