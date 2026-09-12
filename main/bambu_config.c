#include "bambu_config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "mbedtls/base64.h"
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#endif
static const char *field(const cJSON *j, const char *key)
{ const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,key);return cJSON_IsString(v)?v->valuestring:""; }
static bool identifier(const char *text)
{
    if(!*text)return false;
    for(;*text;text++)if(!isalnum((unsigned char)*text) && *text!='_')return false;
    return true;
}
/* Decode only the account ID; authentication and token expiry are checked by the broker. */
static bool token_username(const char *token,char *username,size_t capacity)
{
    const char *start=strchr(token,'.');if(!start)return false;start++;
    const char *end=strchr(start,'.');if(!end)return false;
    size_t n=end-start;if(!n || n>=BAMBU_TOKEN_SIZE-4)return false;
    char encoded[BAMBU_TOKEN_SIZE],decoded[BAMBU_TOKEN_SIZE];
    for(size_t i=0;i<n;i++)encoded[i]=start[i]=='-'?'+':start[i]=='_'?'/':start[i];
    while(n%4)encoded[n++]='=';
    size_t used=0;
    if(mbedtls_base64_decode((unsigned char *)decoded,sizeof(decoded)-1,&used,(unsigned char *)encoded,n))return false;
    decoded[used]=0;cJSON *j=cJSON_ParseWithLength(decoded,used+1);
    const char *keys[]={"uid","sub","user_id"};bool ok=false;
    for(int i=0;i<3 && !ok;i++) {
        const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,keys[i]);char numeric[24];const char *uid=NULL;
        if(cJSON_IsString(v))uid=v->valuestring;
        else if(cJSON_IsNumber(v) && v->valuedouble>=0 && v->valuedouble<=9007199254740991.0 && floor(v->valuedouble)==v->valuedouble) {
            snprintf(numeric,sizeof(numeric),"%.0f",v->valuedouble);uid=numeric;
        }
        if(uid && identifier(uid) && strlen(uid)+3<=capacity){snprintf(username,capacity,"u_%s",uid);ok=true;}
    }
    cJSON_Delete(j);return ok;
}
bool BambuConfig_Parse(const char *json,const BambuConfig *saved,BambuConfig *out,char *error,size_t size)
{
    cJSON *j=cJSON_Parse(json);const char *problem=NULL;BambuConfig next={0};
    if(!cJSON_IsObject(j)){problem="Invalid JSON";goto done;}
    const char *region=field(j,"region");if(!*region)region="cn";
    if(strcmp(region,"cn") && strcmp(region,"us")){problem="Choose China or Global region";goto done;}
    strcpy(next.region,region);
    bool cloud=false;
    for(int i=0;i<2;i++) {
        BambuPrinterConfig *p=&next.printers[i];char key[16];
        snprintf(key,sizeof(key),"mode%d",i);const char *mode=field(j,key);
        snprintf(key,sizeof(key),"ip%d",i);const char *ip=field(j,key);
        snprintf(key,sizeof(key),"serial%d",i);const char *sn=field(j,key);
        snprintf(key,sizeof(key),"code%d",i);const char *code=field(j,key);
        if(!*mode)mode=(*ip || *sn || *code)?"lan":"off"; /* Existing LAN settings. */
        if(!strcmp(mode,"off"))continue;
        if(strcmp(mode,"lan") && strcmp(mode,"cloud")){problem="Invalid printer mode";goto done;}
        if(!identifier(sn) || strlen(sn)>=sizeof(p->serial)){problem="Enter printer serial number";goto done;}
        strcpy(p->serial,sn);p->mode=!strcmp(mode,"cloud")?BAMBU_CLOUD:BAMBU_LAN;
        if(p->mode==BAMBU_CLOUD)cloud=true;
        /* Camera credentials are local even when status uses Cloud MQTT. */
        if(!*code && saved && (p->mode==BAMBU_LAN || *ip) &&
           !strcmp(saved->printers[i].serial,sn))code=saved->printers[i].code;
        if(p->mode==BAMBU_CLOUD && !*ip && !*code)continue;
        struct in_addr addr;
        if(strlen(ip)>=sizeof(p->ip) || inet_pton(AF_INET,ip,&addr)!=1){problem="Enter printer IPv4 address";goto done;}
        strcpy(p->ip,ip);
        if(!*code || strlen(code)>=sizeof(p->code)){problem="Enter LAN access code";goto done;}
        strcpy(p->code,code);
    }
    if(next.printers[0].mode && next.printers[1].mode && !strcmp(next.printers[0].serial,next.printers[1].serial)) {
        problem="Choose two different printer serial numbers";goto done;
    }
    if(cloud) {
        const char *token=field(j,"token"),*user=field(j,"username");
        bool reuse=!*token && saved && !strcmp(saved->region,region);
        if(reuse)token=saved->token;
        if(!*token || strlen(token)>=sizeof(next.token)){problem="Enter Bambu access token (max 2047 characters)";goto done;}
        for(const char *p=token;*p;p++)if(isspace((unsigned char)*p)){problem="Paste the token value without Bearer or whitespace";goto done;}
        strcpy(next.token,token);
        if(!token_username(token,next.username,sizeof(next.username))) {
            if(!*user && reuse)user=saved->username;
            if(strncmp(user,"u_",2) || !identifier(user+2) || strlen(user)>=sizeof(next.username)) {
                problem="Token has no account ID; enter MQTT username u_<account UID>";goto done;
            }
            strcpy(next.username,user);
        }
    }
    *out=next;
done:
    cJSON_Delete(j);if(problem)snprintf(error,size,"%s",problem);return problem==NULL;
}
char *BambuConfig_JSON(const BambuConfig *c,bool secrets)
{
    cJSON *j=cJSON_CreateObject();const char *modes[]={"off","lan","cloud"};
    if(!j)return NULL;
    if(!cJSON_AddStringToObject(j,"region",c->region[0]?c->region:"cn") ||
       !cJSON_AddStringToObject(j,"token",secrets?c->token:"") ||
       !cJSON_AddStringToObject(j,"username",c->username) ||
       !cJSON_AddBoolToObject(j,"has_token",c->token[0]!=0))goto failed;
    for(int i=0;i<2;i++) {
        const BambuPrinterConfig *p=&c->printers[i];char key[16];
        snprintf(key,sizeof(key),"mode%d",i);if(!cJSON_AddStringToObject(j,key,modes[p->mode]))goto failed;
        snprintf(key,sizeof(key),"serial%d",i);if(!cJSON_AddStringToObject(j,key,p->serial))goto failed;
        snprintf(key,sizeof(key),"ip%d",i);if(!cJSON_AddStringToObject(j,key,p->ip))goto failed;
        snprintf(key,sizeof(key),"code%d",i);if(!cJSON_AddStringToObject(j,key,secrets?p->code:""))goto failed;
    }
    char *json=cJSON_PrintUnformatted(j);cJSON_Delete(j);return json;
failed:
    cJSON_Delete(j);return NULL;
}

const char *BambuConfig_Broker(const BambuConfig *c)
{ return !strcmp(c->region,"cn")?"cn.mqtt.bambulab.com":"us.mqtt.bambulab.com"; }
