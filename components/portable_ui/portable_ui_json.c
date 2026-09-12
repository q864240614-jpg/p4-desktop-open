#include "portable_ui.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static float number(const cJSON *object, const char *key, float min, float max)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= min && value->valuedouble <= max ? value->valuedouble : NAN;
}

/* Missing/failed usage retains the last good values with an explicit stale flag.
 * Match windows by duration, not by their primary/secondary ordering. */
static PortableUI_Codex parse_codex(const cJSON *usage, PortableUI_Codex previous)
{
    previous.online = false;
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(usage, "ok"))) return previous;
    PortableUI_Codex next = {.remaining = {NAN, NAN}};
    const char *names[] = {"primary_window", "secondary_window"};
    for (int i = 0; i < 2; ++i) {
        const cJSON *window = cJSON_GetObjectItemCaseSensitive(usage, names[i]);
        const float seconds = number(window, "limit_window_seconds", 1, 604800);
        const int index = seconds == 604800 ? 0 : seconds < 604800 ? 1 : -1;
        const float used = number(window, "used_percent", 0, 100);
        if (index < 0 || !isfinite(used)) continue;
        next.remaining[index] = 100 - used;
        if (index == 1) next.short_seconds = (uint32_t)seconds;
        const cJSON *reset = cJSON_GetObjectItemCaseSensitive(window, "reset_at");
        if (cJSON_IsNumber(reset) && isfinite(reset->valuedouble) &&
            reset->valuedouble > 0 && reset->valuedouble <= UINT32_MAX)
            next.reset_at[index] = (uint32_t)reset->valuedouble;
    }
    if (!isfinite(next.remaining[0]) && !isfinite(next.remaining[1])) return previous;
    next.has_data = next.online = true;
    next.limited = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(usage, "allowed")) ||
                   cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(usage, "limit_reached"));
    const cJSON *plan = cJSON_GetObjectItemCaseSensitive(usage, "plan_type");
    snprintf(next.plan, sizeof(next.plan), "%s", cJSON_IsString(plan) ? plan->valuestring : "--");
    return next;
}


bool PortableUI_ParseStatusJSON(const char *json, PortableUI_Status *status)
{
    if (!status) return false;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!cJSON_IsString(host) || !host->valuestring[0]) {
        cJSON_Delete(root);
        status->nas_online = status->codex.online = false;
        return false;
    }
    status->nas_online = true;
    status->codex = parse_codex(cJSON_GetObjectItemCaseSensitive(root, "codex_usage"), status->codex);
    cJSON_Delete(root);
    return true;
}

/* Status endpoint contains other host metrics; consume only codex_usage. */
bool PortableUI_ParseCodexJSON(const char *json, PortableUI_Codex *state)
{
    cJSON *root=json?cJSON_Parse(json):NULL;
    *state=parse_codex(cJSON_GetObjectItemCaseSensitive(root,"codex_usage"),*state);
    cJSON_Delete(root);
    return state->online;
}
