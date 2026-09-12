/* stg_cur definitions: BambuStudio src/slic3r/GUI/DeviceManager.cpp, get_stage_string. */
typedef enum { FX_STILL,FX_IDLE,FX_WATER,FX_HEAT,FX_LEVEL,FX_FEED,FX_SCAN,FX_CLEAN,FX_HOME,FX_FLOW,FX_CALIBRATE,FX_COOL,FX_TOOL,FX_CENTER,FX_CAMERA,FX_LIDAR,FX_MOTOR,FX_MOTION,FX_OFFSET,FX_VIBRATION,FX_VENT,FX_PREP,FX_PAUSE,FX_ERROR,FX_FINISH } PrinterEffect;
static PrinterEffect printer_effect(const BambuState *s)
{
    if(!s->online)return FX_STILL;
    if(s->error)return FX_ERROR;
    if(!strcmp(s->state,"FAILED"))return FX_STILL;
    if(!strcmp(s->state,"PAUSE"))return FX_PAUSE;
    if(!strcmp(s->state,"FINISH"))return FX_FINISH;
    if(!strcmp(s->state,"IDLE"))return FX_IDLE;
    switch(s->stage){
    case 1:case 40:case 47:case 48:return FX_LEVEL;
    case 2:case 7:case 15:case 49:case 50:case 54:return FX_HEAT;
    case 4:case 22:case 24:return FX_FEED;
    case 9:case 10:case 11:case 41:case 42:case 44:case 45:case 52:case 55:return FX_SCAN;
    case 14:return FX_CLEAN;
    case 13:return FX_HOME;
    case 8:return FX_FLOW;
    case 3:return FX_VIBRATION;
    case 12:case 18:return FX_LIDAR;
    case 25:case 31:return FX_MOTOR;
    case 36:case 37:case 38:return FX_MOTION;
    case 39:case 56:return FX_OFFSET;
    case 46:case 53:case 60:return FX_CAMERA;
    case 62:return FX_TOOL;
    case 70:return FX_CENTER;
    case 19:case 43:case 51:return FX_CALIBRATE;
    case 29:return FX_COOL;
    case 66:return FX_VENT;
    case 5:case 6:case 16:case 17:case 20:case 21:case 23:case 26:case 27:case 28:case 30:case 32:case 33:case 34:case 35:return FX_PAUSE;
    case 0:case -1:case 255:return !strcmp(s->state,"RUNNING") && s->progress>0?FX_WATER:FX_PREP;
    default:return FX_WATER;
    }
}
static const char *printer_status(const BambuState *s,char *buffer,size_t size)
{
    (void)buffer;(void)size;
    if(!s->online)return s->has_data?"DISCONNECTED":"OFFLINE";
    if(s->error)return "FAILED";
    if(!strcmp(s->state,"FAILED"))return "STOPPED";
    if(!strcmp(s->state,"PAUSE") || !strcmp(s->state,"FINISH") || !strcmp(s->state,"IDLE"))return s->state;
    switch(s->stage){
    case 1:return "BED LEVELING";case 40:return "HOT LEVELING";case 47:return "LEVELING 1";case 48:return "LEVELING 2";
    case 2:return "BED HEATING";case 7:return "NOZZLE HEAT";case 15:return "TEMP CHECK";case 49:return "CHAMBER HEAT";case 50:case 54:return "BED TEMP";
    case 4:return "FILAMENT SWAP";case 22:return "UNLOADING";case 24:return "LOADING";
    case 3:return "VIBRATION CAL";case 8:return "FLOW CAL";case 19:return "FLOW RATIO";
    case 9:return "BED SCANNING";case 10:return "FIRST LAYER";case 11:return "PLATE CHECK";
    case 12:case 18:return "LIDAR CAL";case 13:return "HOMING";case 14:return "NOZZLE WIPE";
    case 25:case 31:return "MOTOR CAL";case 29:return "COOLING";
    case 36:case 37:case 38:return "MOTION CAL";case 39:return "OFFSET CAL";
    case 41:case 42:case 44:case 52:case 55:return "SELF TEST";
    case 43:return "LASER CAL";case 46:case 53:case 60:return "CAMERA CAL";case 56:return "OFFSET CAL";
    case 62:return "HOTEND TEST";case 70:return "CENTERING";case 45:return "CAMERA ALIGN";case 51:return "CAL LINE";
    case 66:return "AIR PURIFY";
    case 0:case -1:case 255:return s->state[0]?s->state:"CONNECTED";
    default:if(printer_effect(s)==FX_PAUSE)return "WAITING";return "RUNNING";
    }
}
static uint32_t printer_effect_color(PrinterEffect effect)
{
    switch(effect){
    case FX_STILL:return COLOR_MUTED;
    case FX_IDLE:case FX_FINISH:return 0x4A9AD4;
    case FX_ERROR:return 0xEF4248;case FX_PAUSE:return 0xF2B942;
    case FX_HEAT:return 0xEDAA48;case FX_LEVEL:case FX_SCAN:return 0x419DCE;
    case FX_FEED:case FX_COOL:case FX_VENT:return 0x37BBA4;
    case FX_TOOL:case FX_CENTER:case FX_CAMERA:case FX_LIDAR:case FX_MOTOR:case FX_MOTION:case FX_OFFSET:case FX_VIBRATION:
    case FX_CALIBRATE:case FX_HOME:case FX_FLOW:return 0x869EE0;
    case FX_WATER:case FX_PREP:return COLOR_BAMBU;
    default:return COLOR_BAMBU;
    }
}
static uint32_t printer_color(const BambuState *s)
{ return printer_effect_color(printer_effect(s)); }
static bool printer_running(const BambuState *s)
{ PrinterEffect e=printer_effect(s);return e>=FX_WATER && e<=FX_PREP; }
