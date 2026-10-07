/* SPDX-License-Identifier: GPL-3.0-only */
#include "cJSON.h"
#include "core/core.h"
#include <limits.h>
#include <math.h>

/* cJSON stores numbers as doubles. Reject fractions/out-of-range values
 * instead of silently accepting its saturated/truncated valueint member.
 */
static bool get_int(const cJSON *json, const char *key, int *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < INT_MIN ||
        value->valuedouble > INT_MAX || value->valuedouble != (int)value->valuedouble) {
        return false;
    }
    *out = (int)value->valuedouble;
    return true;
}

bool mainui_state_parse(const char *text, MainUIStack *out)
{
    if (!text || !out) {
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(text, NULL, true);
    if (!root) {
        return false;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "list");
    /* Parse into temporary owned state: a malformed later frame must not leave
     * the caller with a partially replaced navigation stack.
     */
    MainUIStack parsed = {0};
    bool ok = cJSON_IsObject(root) && cJSON_IsArray(list);
    const cJSON *item;
    cJSON_ArrayForEach(item, list)
    {
        if (!ok || parsed.count == MAINUI_STACK_MAX) {
            ok = false;
            break;
        }
        MainUIFrame *frame = &parsed.frames[parsed.count];
        ok = cJSON_IsObject(item) && get_int(item, "title", &frame->title) &&
             get_int(item, "type", &frame->type) && get_int(item, "currpos", &frame->selected) &&
             get_int(item, "pagestart", &frame->start) && get_int(item, "pageend", &frame->end);
        if (!ok) {
            break;
        }
        parsed.count++;
    }
    cJSON_Delete(root);
    if (ok) {
        *out = parsed;
    }
    return ok;
}

char *mainui_state_json(const MainUIStack *state)
{
    if (!state || state->count > MAINUI_STACK_MAX) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject(), *list = cJSON_CreateArray();
    if (!root || !list) {
        cJSON_Delete(root);
        cJSON_Delete(list);
        return NULL;
    }
    /* cJSON takes ownership only after a successful attachment. Every failure
     * branch frees unattached nodes as well as the accumulated tree.
     */
    if (!cJSON_AddItemToObject(root, "list", list)) {
        cJSON_Delete(list);
        cJSON_Delete(root);
        return NULL;
    }
    for (size_t i = 0; i < state->count; i++) {
        const MainUIFrame *f = &state->frames[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return NULL;
        }
        bool ok = cJSON_AddNumberToObject(item, "title", f->title) &&
                  cJSON_AddNumberToObject(item, "type", f->type) &&
                  cJSON_AddNumberToObject(item, "currpos", f->selected) &&
                  cJSON_AddNumberToObject(item, "pagestart", f->start) &&
                  cJSON_AddNumberToObject(item, "pageend", f->end);
        if (!ok || !cJSON_AddItemToArray(list, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return NULL;
        }
    }
    /* One field per line, as stock writes it: Onion's Game List Options reads
     * the second "type" line with grep and sed to tell which list the game
     * came from, and on one line it finds none. */
    char *result = cJSON_Print(root);
    cJSON_Delete(root);
    return result;
}
