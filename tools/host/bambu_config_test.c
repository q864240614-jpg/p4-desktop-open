#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bambu_config.h"
int main(void)
{
    BambuConfig saved={0},next={0};char error[128];
    const char *cloud="{\"mode0\":\"cloud\",\"serial0\":\"H2D123\",\"mode1\":\"cloud\",\"serial1\":\"A1MINI456\",\"region\":\"cn\",\"token\":\"a.eyJ1aWQiOiIxMjM0NSJ9.c\"}";
    assert(BambuConfig_Parse(cloud,NULL,&saved,error,sizeof(error)));
    assert(!strcmp(saved.username,"u_12345") && !strcmp(BambuConfig_Broker(&saved),"cn.mqtt.bambulab.com"));
    char *json=BambuConfig_JSON(&saved,false);
    assert(!strstr(json,saved.token));
    assert(BambuConfig_Parse(json,&saved,&next,error,sizeof(error)) && !strcmp(saved.token,next.token));
    cJSON *j=cJSON_Parse(json);free(json);cJSON_ReplaceItemInObject(j,"region",cJSON_CreateString("us"));json=cJSON_PrintUnformatted(j);
    assert(!BambuConfig_Parse(json,&saved,&next,error,sizeof(error)));free(json);cJSON_Delete(j);
    json=BambuConfig_JSON(&saved,true);assert(BambuConfig_Parse(json,NULL,&next,error,sizeof(error)));
    assert(!memcmp(&next,&saved,sizeof(next)));free(json);
    assert(BambuConfig_Parse("{\"ip0\":\"192.168.3.5\",\"serial0\":\"H2D123\",\"code0\":\"12345678\",\"ip1\":\"\",\"serial1\":\"\",\"code1\":\"\"}",NULL,&next,error,sizeof(error)));
    assert(next.printers[0].mode==BAMBU_LAN && next.printers[1].mode==BAMBU_OFF);
    saved=next;json=BambuConfig_JSON(&saved,false);assert(!strstr(json,"12345678"));
    assert(BambuConfig_Parse(json,&saved,&next,error,sizeof(error)) && !strcmp(next.printers[0].code,"12345678"));free(json);
    assert(!BambuConfig_Parse("{\"mode0\":\"cloud\",\"serial0\":\"H2D123\",\"region\":\"cn\",\"token\":\"bad.token.value\"}",NULL,&next,error,sizeof(error)));
    assert(BambuConfig_Parse("{\"mode0\":\"cloud\",\"serial0\":\"H2D123\",\"region\":\"us\",\"token\":\"opaque\",\"username\":\"u_12345\"}",NULL,&next,error,sizeof(error)));
    assert(!strcmp(BambuConfig_Broker(&next),"us.mqtt.bambulab.com"));
    assert(BambuConfig_Parse("{\"mode0\":\"cloud\",\"serial0\":\"H2D123\",\"ip0\":\"192.168.3.5\",\"code0\":\"12345678\",\"region\":\"us\",\"token\":\"opaque\",\"username\":\"u_12345\"}",NULL,&saved,error,sizeof(error)));
    assert(saved.printers[0].mode==BAMBU_CLOUD && !strcmp(saved.printers[0].ip,"192.168.3.5"));
    json=BambuConfig_JSON(&saved,false);assert(!strstr(json,"12345678"));
    assert(BambuConfig_Parse(json,&saved,&next,error,sizeof(error)));
    assert(!strcmp(next.printers[0].code,"12345678"));free(json);
    assert(!BambuConfig_Parse("{\"mode0\":\"cloud\",\"serial0\":\"H2D123\",\"mode1\":\"cloud\",\"serial1\":\"H2D123\"}",NULL,&next,error,sizeof(error)));
    puts("PASS: cloud identity, region, persistence, redaction, legacy LAN and distinct slots");
}
