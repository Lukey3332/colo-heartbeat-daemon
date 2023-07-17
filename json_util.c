/*
 * COLO background daemon json utilities
 *
 * Copyright (c) Lukas Straub <lukasstraub2@web.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <assert.h>

#include <glib-2.0/glib.h>
#include <json-glib-1.0/json-glib/json-glib.h>

#include "util.h"

const gchar *bool_to_json(gboolean bool) {
    if (bool) {
        return "True";
    } else {
        return "False";
    }
}

gboolean has_member(JsonNode *node, const gchar *member) {
    JsonObject *object;

    assert(JSON_NODE_HOLDS_OBJECT(node));
    object = json_node_get_object(node);
    return json_object_has_member(object, member);
}

const gchar *get_member_str(JsonNode *node, const gchar *member) {
    JsonObject *object;

    assert(JSON_NODE_HOLDS_OBJECT(node));
    object = json_node_get_object(node);
    return json_object_get_string_member(object, member);
}

JsonNode *get_member_node(JsonNode *node, const gchar *member) {
    JsonObject *object;

    assert(JSON_NODE_HOLDS_OBJECT(node));
    object = json_node_get_object(node);
    return json_object_get_member(object, member);
}

const gchar *get_member_member_str(JsonNode *node, const gchar *member1,
                                   const gchar *member2) {
    JsonObject *object;

    assert(JSON_NODE_HOLDS_OBJECT(node));
    object = json_node_get_object(node);
    object = json_object_get_object_member(object, member1);
    return json_object_get_string_member(object, member2);
}

gboolean object_matches(JsonNode *node, JsonNode *match) {
    JsonObject *object, *match_object;
    JsonObjectIter iter;
    const gchar *match_member;
    JsonNode *match_node;

    assert(JSON_NODE_HOLDS_OBJECT(match));

    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        return FALSE;
    }

    object = json_node_get_object(node);
    match_object = json_node_get_object(match);

    json_object_iter_init (&iter, match_object);
    while (json_object_iter_next (&iter, &match_member, &match_node))
    {
        if (!json_object_has_member(object, match_member)) {
            return FALSE;
        }

        JsonNode *member_node = json_object_get_member(object, match_member);
        if (!json_node_equal(member_node, match_node)) {
            return FALSE;
        }
    }

    return TRUE;
}

gboolean object_matches_match_array(JsonNode *node, JsonNode *match_array) {
    JsonReader *reader;

    assert(JSON_NODE_HOLDS_ARRAY(match_array));

    reader = json_reader_new(match_array);

    guint count = json_reader_count_elements(reader);
    for (guint i = 0; i < count; i++) {
        json_reader_read_element(reader, i);
        JsonNode *match = json_reader_get_value(reader);
        assert(match);

        if (object_matches(node, match)) {
            g_object_unref(reader);
            return TRUE;
        }

        json_reader_end_element(reader);
    }
    json_reader_end_member(reader);
    g_object_unref(reader);

    return FALSE;
}