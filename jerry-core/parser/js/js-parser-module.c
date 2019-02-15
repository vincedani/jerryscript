/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jcontext.h"
#include "jerryscript.h"
#include "js-parser.h"
#include "js-parser-internal.h"
#include "jerryscript-port.h"

#include "ecma-function-object.h"
#include "ecma-gc.h"
#include "ecma-globals.h"
#include "ecma-helpers.h"
#include "ecma-lex-env.h"

#include "module.h"

#ifndef CONFIG_DISABLE_ES2015_MODULE_SYSTEM
/**
 * Maximum import count limit.
 */
#define MAX_IMPORT_COUNT UINT16_MAX

/**
 * Check duplicates in module node.
 * @return true - if the given item is duplicated entry in the current node
 *         false - otherwise
 */
static bool
parser_module_check_for_duplicates_in_node (parser_module_node_t *module_node_p, /**< module node */
                                            lexer_literal_t *import_name_p) /**< newly imported name */
{
  JERRY_ASSERT (import_name_p != NULL);

  parser_module_names_t *import_names_p = module_node_p->module_names_p;

  while (import_names_p != NULL)
  {
    uint8_t *current_p = import_names_p->import_name.value_p;
    prop_length_t current_length = import_names_p->import_name.length;

    if (current_p != NULL && current_length == import_name_p->prop.length)
    {
      if (memcmp (current_p, import_name_p->u.char_p, current_length) == 0)
      {
        return true;
      }
    }
    import_names_p = import_names_p->next_p;
  }

  return false;
} /* parser_module_check_for_duplicates_in_node */

/**
 * Check duplicates in the whole module context.
 * @return true - if the given item is duplicated entry
 *         false - otherwise
 */
static bool
parser_module_check_for_duplicates (parser_context_t *context_p, /**< parser context */
                                    lexer_literal_t *import_name_p) /**< newly imported name */
{
  if (import_name_p == NULL)
  {
    return false;
  }

  bool hasDuplicate = parser_module_check_for_duplicates_in_node (context_p->module_current_node_p,
                                                                  import_name_p);

  if (!hasDuplicate)
  {
    parser_module_node_t *node_p = context_p->module_context_p->imports_p;
    while (node_p != NULL && !hasDuplicate)
    {
      hasDuplicate = parser_module_check_for_duplicates_in_node (node_p,
                                                                 import_name_p);
      node_p = node_p->next_p;
    }
  }

  return hasDuplicate;
} /* parser_module_check_for_duplicates */

/**
 * Check if the import statement contains valid aliases.
 */
static void
parser_module_check_valid_aliases (parser_context_t *context_p) /**< parser context */
{
  parser_module_node_t *imports_p = context_p->module_context_p->imports_p;
  if (imports_p == NULL)
  {
    return;
  }

  parser_module_names_t collective_name;
  bool is_whole_module_requested = parser_module_is_whole_module_requested (imports_p, &collective_name);

  if (!is_whole_module_requested ||
      (is_whole_module_requested && collective_name.import_name.value_p != NULL))
  {
    return;
  }

  parser_module_names_t *import_name_p = imports_p->module_names_p;
  for (uint16_t i = 0; i < imports_p->module_request_count; ++i)
  {
    if (import_name_p->local_name.value_p != NULL
       && !import_name_p->is_default_item
       && (import_name_p->import_name.length == import_name_p->local_name.length)
       && memcmp (import_name_p->local_name.value_p,
                  import_name_p->import_name.value_p,
                  import_name_p->local_name.length) == 0)
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_ALIASES);
    }

    import_name_p = import_name_p->next_p;
  }
} /* parser_module_check_valid_aliases */

/**
 * Check for default duplicates in the whole module context.
 * @return - true - if default export was declared previously
 *           false - otherwise
 */
static bool
parser_module_check_for_default_exports (parser_context_t *context_p) /**< parser context */
{
  if (!context_p->module_processing_default_item || context_p->module_context_p->exports_p == NULL)
  {
    return false;
  }

  parser_module_names_t *names_iterator_p = context_p->module_context_p->exports_p->module_names_p;

  while (names_iterator_p != NULL)
  {
    if (names_iterator_p->is_default_item)
    {
      return true;
    }

    names_iterator_p = names_iterator_p->next_p;
  }

  return false;
} /* parser_module_check_for_default_duplicates */

/**
 * Delete the saved names from the given module node.
 */
void
parser_module_free_saved_names (parser_module_node_t *module_node_p, /**< module node */
                                bool is_forced_delete) /**< if the operation is forced */
{
  if (module_node_p == NULL || module_node_p->module_names_p == NULL)
  {
    return;
  }

  parser_module_names_t *current_p = module_node_p->module_names_p;

  for (uint16_t i = 0; i < module_node_p->module_request_count; i++)
  {
    parser_module_names_t *next_p = current_p->next_p;

    if (!current_p->is_redirected_item || is_forced_delete)
    {
      if (current_p->import_name.value_p != NULL)
      {
        parser_free (current_p->import_name.value_p, current_p->import_name.length * sizeof (uint8_t));
        current_p->import_name.value_p = NULL;
      }

      if (current_p->local_name.value_p != NULL)
      {
        parser_free (current_p->local_name.value_p, current_p->local_name.length * sizeof (uint8_t));
        current_p->local_name.value_p = NULL;
      }

      parser_free (current_p, sizeof (parser_module_names_t));
    }
    current_p = next_p;
  }
} /* parser_module_free_saved_names */

/**
 * Add export node to parser context.
 */
void
parser_module_add_export_node_to_context (parser_context_t *context_p) /**< parser context */
{
  parser_module_node_t *module_node_p = context_p->module_current_node_p;
  parser_module_node_t *exports_p = context_p->module_context_p->exports_p;

  if (exports_p != NULL)
  {
    parser_module_names_t *module_names_p = module_node_p->module_names_p;

    for (uint16_t i = 0; i < module_node_p->module_request_count - 1 ; i++)
    {
      module_names_p = module_names_p->next_p;
    }

    module_names_p->next_p = exports_p->module_names_p;
    exports_p->module_names_p = module_node_p->module_names_p;

    int request_count = exports_p->module_request_count + module_node_p->module_request_count;

    if (request_count < MAX_IMPORT_COUNT)
    {
      exports_p->module_request_count = (uint16_t) request_count;
    }
    else
    {
      parser_raise_error (context_p, PARSER_ERR_MODULE_REQUEST_LIMIT_REACHED);
    }
  }
  else
  {
    parser_module_node_t *export_node_p = parser_module_create_module_node (context_p, module_node_p);
    context_p->module_context_p->exports_p = export_node_p;
  }
} /* parser_module_add_export_node_to_context */

/**
 * Add import node to parser context.
 */
void
parser_module_add_import_node_to_context (parser_context_t *context_p) /**< parser context */
{
  parser_module_node_t *module_node_p = context_p->module_current_node_p;
  parser_module_node_t *stored_imports = context_p->module_context_p->imports_p;
  bool is_stored_module = false;

  while (stored_imports != NULL)
  {
    if (stored_imports->script_path.length == module_node_p->script_path.length
       && memcmp (stored_imports->script_path.value_p,
                  module_node_p->script_path.value_p,
                  stored_imports->script_path.length) == 0)
    {
      parser_free (module_node_p->script_path.value_p, module_node_p->script_path.length * sizeof (uint8_t));

      parser_module_names_t *module_names_p = module_node_p->module_names_p;
      is_stored_module = true;

      for (uint16_t i = 0; i < module_node_p->module_request_count - 1; i++)
      {
        module_names_p = module_names_p->next_p;
      }

      module_names_p->next_p = stored_imports->module_names_p;
      stored_imports->module_names_p = module_node_p->module_names_p;

      int request_count = stored_imports->module_request_count + module_node_p->module_request_count;
      if (request_count < MAX_IMPORT_COUNT)
      {
        stored_imports->module_request_count = (uint16_t) request_count;
      }
      else
      {
        parser_raise_error (context_p, PARSER_ERR_MODULE_REQUEST_LIMIT_REACHED);
      }

      break;
    }
    stored_imports = stored_imports->next_p;
  }

  if (!is_stored_module)
  {
    parser_module_node_t *permanent_node_p = parser_module_create_module_node (context_p, module_node_p);
    permanent_node_p->next_p = context_p->module_context_p->imports_p;
    context_p->module_context_p->imports_p = permanent_node_p;
  }

} /* parser_module_add_import_node_to_context */

/**
 * Add import or export item to module node.
 */
void
parser_module_add_item_to_node (parser_context_t *context_p, /**< parser context */
                                lexer_literal_t *import_name_p, /**< import name */
                                lexer_literal_t *local_name_p, /**< local name */
                                bool is_import_item) /**< given item is import item */
{
  JERRY_ASSERT (context_p->module_current_node_p != NULL);

  if (is_import_item && parser_module_check_for_duplicates (context_p, import_name_p))
  {
    parser_raise_error (context_p, PARSER_ERR_DUPLICATED_LABEL);
  }

  if (!is_import_item && parser_module_check_for_default_exports (context_p))
  {
    parser_raise_error (context_p, PARSER_ERR_DUPLICATED_DEFAULT_ITEM);
  }

  parser_module_node_t *module_node_p = context_p->module_current_node_p;
  parser_module_names_t *new_names_p =
  (parser_module_names_t *) parser_malloc (context_p, sizeof (parser_module_names_t));

  new_names_p->next_p = module_node_p->module_names_p;
  module_node_p->module_names_p = new_names_p;
  prop_length_t length;

  /* An empty record if the whole module is requested */
  if (import_name_p == NULL)
  {
    new_names_p->import_name.value_p = NULL;
    new_names_p->import_name.length = 0;
  }
  else
  {
    length = import_name_p->prop.length;
    new_names_p->import_name.length = length;
    new_names_p->import_name.value_p = (uint8_t *) parser_malloc (context_p, length * sizeof (uint8_t));
    memcpy (new_names_p->import_name.value_p, import_name_p->u.char_p, length);
  }

  if (local_name_p == NULL)
  {
    new_names_p->local_name.value_p = NULL;
    new_names_p->local_name.length = 0;
  }
  else
  {
    length = local_name_p->prop.length;
    new_names_p->local_name.length = length;
    new_names_p->local_name.value_p = (uint8_t *) parser_malloc (context_p, length * sizeof (uint8_t));
    memcpy (new_names_p->local_name.value_p, local_name_p->u.char_p, length);
  }

  new_names_p->is_default_item = context_p->module_processing_default_item;
  module_node_p->module_request_count++;

  // Reseting default item's indicator in case of parsing import item list.
   context_p->module_processing_default_item = false;
} /* parser_module_add_item_to_node */

/**
 * Cleanup the whole module context from parser context.
 */
void
parser_module_context_cleanup (parser_context_t *context_p) /**< parser context */
{
  parser_module_context_t *module_context_p = context_p->module_context_p;

  if (module_context_p == NULL)
  {
    return;
  }

  parser_module_node_t *current_node_p = module_context_p->imports_p;

  while (current_node_p != NULL)
  {
    parser_free (current_node_p->script_path.value_p, current_node_p->script_path.length * sizeof (uint8_t));
    parser_module_free_saved_names (current_node_p, true);

    parser_module_node_t *next_node_p = current_node_p->next_p;

    parser_free (current_node_p, sizeof (parser_module_node_t));
    current_node_p = next_node_p;
  }

  parser_module_context_t *parent_context_p = JERRY_CONTEXT (module_top_context_p);
  if ((parent_context_p == NULL || parent_context_p->exports_p == NULL || parent_context_p->imports_p == NULL)
    && module_context_p->exports_p != NULL)
  {
    parser_module_free_saved_names (module_context_p->exports_p, false);
    parser_free (module_context_p->exports_p, sizeof (parser_module_node_t));
  }

  parser_free (module_context_p, sizeof (parser_module_context_t));
  context_p->module_context_p = NULL;
} /* parser_module_context_cleanup */

/**
 * Create module context and bind to the parser context.
 */
void
parser_module_context_init (parser_context_t *context_p) /**< parser context */
{
  if (context_p->module_context_p == NULL)
  {
    context_p->module_context_p = (parser_module_context_t *) parser_malloc (context_p,
                                                                             sizeof (parser_module_context_t));

    memset (context_p->module_context_p, 0, sizeof (parser_module_context_t));
  }
  context_p->module_processing_default_item = false;
} /* parser_module_context_init */

/**
 * Create import node.
 * @return - the copy of the temlpate if the second parameter is not NULL.
 *         - otherwise: an empty import node.
 */
parser_module_node_t *
parser_module_create_module_node (parser_context_t *context_p, /**< parser context */
                                  parser_module_node_t *template_node_p) /**< template node for the new node */
{
  parser_module_node_t *node_p = (parser_module_node_t *) parser_malloc (context_p, sizeof (parser_module_node_t));

  if (template_node_p != NULL)
  {
    node_p->module_names_p = template_node_p->module_names_p;
    node_p->module_request_count = template_node_p->module_request_count;

    node_p->script_path.value_p = template_node_p->script_path.value_p;
    node_p->script_path.length = template_node_p->script_path.length;

    node_p->is_import_for_side_effect = template_node_p->is_import_for_side_effect;
    node_p->next_p = NULL;
  }
  else
  {
    memset (node_p, 0, sizeof (parser_module_node_t));
  }

  return node_p;
} /* parser_module_create_module_node */

/**
 * Create export node or get the previously created one.
 * @return the export node
 */
parser_module_node_t *
parser_module_get_export_node (parser_context_t *context_p) /**< parser context */
{
  if (context_p->module_context_p->exports_p != NULL)
  {
    return context_p->module_context_p->exports_p;
  }

  return parser_module_create_module_node (context_p, NULL);
} /* parser_module_get_export_node */

/**
 * Parse export item list.
 */
void
parser_module_parse_export_item_list (parser_context_t *context_p) /**< parser context */
{
  if (context_p->token.type == LEXER_LITERAL
      && lexer_compare_raw_identifier_to_current (context_p, "from", 4))
  {
    parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
  }

  if (context_p->token.type == LEXER_KEYW_DEFAULT || context_p->token.type == LEXER_MULTIPLY)
  {
    /* TODO: This part is going to be implemented in the next part of the patch. */
    parser_raise_error (context_p, PARSER_ERR_NOT_IMPLEMENTED);
  }

  bool has_export_name = false;
  lexer_literal_t *export_name_p = NULL;
  lexer_literal_t *local_name_p = NULL;

  while (true)
  {
    if (has_export_name
        && context_p->token.type != LEXER_KEYW_DEFAULT
        && (context_p->token.type != LEXER_LITERAL
            || lexer_compare_raw_identifier_to_current (context_p, "from", 4)
            || lexer_compare_raw_identifier_to_current (context_p, "as", 2)))
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
    }

    if (context_p->token.lit_location.type != LEXER_IDENT_LITERAL
        && context_p->token.lit_location.type != LEXER_STRING_LITERAL)
    {
      parser_raise_error (context_p, PARSER_ERR_PROPERTY_IDENTIFIER_EXPECTED);
    }

    if (context_p->token.type == LEXER_KEYW_DEFAULT)
    {
      parser_module_set_default (context_p);
    }
    else
    {
      lexer_construct_literal_object (context_p, &context_p->token.lit_location, LEXER_STRING_LITERAL);

      if (has_export_name)
      {
        export_name_p = context_p->lit_object.literal_p;
      }
      else
      {
        local_name_p = context_p->lit_object.literal_p;
        export_name_p = context_p->lit_object.literal_p;
      }
    }

    lexer_next_token (context_p);

    if (context_p->token.type == LEXER_COMMA)
    {
      has_export_name = false;
      parser_module_add_item_to_node (context_p, export_name_p, local_name_p, false);
    }
    else if (context_p->token.type == LEXER_LITERAL
             && lexer_compare_raw_identifier_to_current (context_p, "as", 2))
    {
      if (has_export_name)
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
      }

      has_export_name = true;
    }
    else
    {
      parser_module_add_item_to_node (context_p, export_name_p, local_name_p, false);
      break;
    }
    lexer_next_token (context_p);
  }
} /* parser_module_parse_export_item_list */

/**
 * Parse import item list.
 */
void
parser_module_parse_import_item_list (parser_context_t *context_p) /**< parser context */
{
  /* Import list is empty, the request is for the side effect only. */
  if (context_p->token.type == LEXER_LITERAL
      && lexer_compare_raw_identifier_to_current (context_p, "from", 4))
  {
    context_p->module_current_node_p->is_import_for_side_effect = true;
    parser_module_add_item_to_node (context_p, NULL, NULL, true);
    return;
  }

  bool has_import_name = false;
  bool processed_default_item = false;
  bool processing_whole_module_request = false;

  lexer_literal_t *import_name_p = NULL;
  lexer_literal_t *local_name_p = NULL;

  while (true)
  {
    if (context_p->token.type == LEXER_LEFT_BRACE)
    {
      if (context_p->module_processing_default_item)
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
      }

      lexer_next_token (context_p);
      parser_module_parse_import_item_list (context_p);

      if (context_p->token.type != LEXER_RIGHT_BRACE)
      {
        parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_EXPECTED);
      }

      lexer_next_token (context_p);
      return;
    }

    bool whole_module_needed = context_p->token.type == LEXER_MULTIPLY;

    if (!whole_module_needed && !processing_whole_module_request && processed_default_item) {
      parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_EXPECTED);
    }

    if ((!whole_module_needed || has_import_name)
        && (context_p->token.type != LEXER_LITERAL
        || lexer_compare_raw_identifier_to_current (context_p, "from", 4)
        || lexer_compare_raw_identifier_to_current (context_p, "as", 2)))
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
    }

    if (whole_module_needed)
    {
      context_p->module_processing_default_item = false;
      processing_whole_module_request = true;
      local_name_p = NULL;
    }
    else
    {
      lexer_construct_literal_object (context_p, &context_p->token.lit_location, LEXER_IDENT_LITERAL);

      if (has_import_name)
      {
        import_name_p = context_p->lit_object.literal_p;
      }
      else
      {
        local_name_p = context_p->lit_object.literal_p;
        import_name_p = context_p->lit_object.literal_p;
      }
    }

    lexer_next_token (context_p);

    if (context_p->token.type == LEXER_RIGHT_BRACE
        || (context_p->token.type == LEXER_LITERAL
        && lexer_compare_raw_identifier_to_current (context_p, "from", 4)))
    {
      parser_module_add_item_to_node (context_p, import_name_p, local_name_p, true);
      break;
    }

    if (context_p->token.type == LEXER_COMMA)
    {
      processed_default_item = context_p->module_processing_default_item;

      parser_module_add_item_to_node (context_p, import_name_p, local_name_p, true);

      has_import_name = false;
      processing_whole_module_request = false;

      import_name_p = NULL;
      local_name_p = NULL;
    }
    else if (context_p->token.type == LEXER_LITERAL
             && lexer_compare_raw_identifier_to_current (context_p, "as", 2))
    {
      if (has_import_name)
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
      }

      has_import_name = true;
    }
    else
    {
      parser_raise_error (context_p, PARSER_ERR_RIGHT_PAREN_COMMA_FROM_EXPECTED);
    }
    lexer_next_token (context_p);
  }
} /* parser_module_parse_import_item_list */

bool
parser_module_is_whole_module_requested (parser_module_node_t *module_node_p, /**< module node */
                                         parser_module_names_t *eventual_names_p) /**< [out] used names */
{
  parser_module_names_t *current_p = module_node_p->module_names_p;

  if (current_p == NULL)
  {
    return false;
  }

  for (uint16_t i = 0; i < module_node_p->module_request_count; i++)
  {
    parser_module_names_t *next_p = current_p->next_p;

    if (current_p->local_name.value_p == NULL)
    {
      *eventual_names_p = *current_p;
      return true;
    }

    current_p = next_p;
  }

  return false;
} /* parser_module_is_whole_module_requested */

/**
 * Handle import requests.
 * Check if imported variables are exported in the appropriate module.
 * Raise parser error if imported item is not exported.
 */
void
parser_module_handle_requests (parser_context_t *context_p) /**< parser context */
{
  parser_module_check_valid_aliases (context_p);

  parser_module_context_t *parent_context_p = JERRY_CONTEXT (module_top_context_p);

  if (context_p->module_context_p->exports_p == NULL || parent_context_p == NULL)
  {
    return;
  }

  bool throw_error = false;

  parser_module_names_t *import_name_p = parent_context_p->imports_p->module_names_p;
  parser_module_names_t *current_exports_p = context_p->module_context_p->exports_p->module_names_p;

  for (uint16_t i = 0; i < parent_context_p->imports_p->module_request_count; ++i)
  {
    bool request_is_found_in_module = false;
    parser_module_names_t *export_iterator_p = current_exports_p;

    /* Whole module is requested, so searching in exports is unnecessary. */
    if (import_name_p->local_name.value_p == NULL)
    {
      request_is_found_in_module = true;
      break;
    }

    for (uint16_t j = 0; j < context_p->module_context_p->exports_p->module_request_count;
         ++j, export_iterator_p = export_iterator_p->next_p)
    {
      if ((import_name_p->local_name.length == export_iterator_p->import_name.length
           && memcmp (import_name_p->local_name.value_p,
                      export_iterator_p->import_name.value_p,
                      import_name_p->local_name.length) == 0)
           || (import_name_p->is_default_item && export_iterator_p->is_default_item))
      {
        request_is_found_in_module = true;
        break;
      }
    }

    if (!request_is_found_in_module)
    {
      parser_module_free_saved_names (context_p->module_context_p->exports_p, false);
      throw_error = true;
      break;
    }

    import_name_p = import_name_p->next_p;
  }

  *parent_context_p->exports_p = *context_p->module_context_p->exports_p;
  parser_free (context_p->module_context_p->exports_p, sizeof (parser_module_node_t));

  if (throw_error)
  {
    parser_raise_error (context_p, PARSER_ERR_MODULE_REQUEST_NOT_FOUND);
  }
} /* parser_module_handle_requests */

/**
 * Raises parser error if the import or export statement is not in the global scope.
 */
void
parser_module_check_request_place (parser_context_t *context_p) /**< parser context */
{
  if (context_p->last_context_p != NULL
      || context_p->stack_top_uint8 != 0
      || (JERRY_CONTEXT (status_flags) & ECMA_STATUS_DIRECT_EVAL) != 0)
  {
    parser_raise_error (context_p, PARSER_ERR_MODULE_UNEXPECTED);
  }
} /* parser_module_check_request_place */

/**
 * Handle from clause at the end of the import / export statement.
 */
void
parser_module_handle_from_clause (parser_context_t *context_p) /**< parser context */
{
  parser_module_node_t *module_node_p = context_p->module_current_node_p;
  lexer_expect_object_literal_id (context_p, LEXER_OBJ_IDENT_NO_OPTS);

  if (context_p->lit_object.literal_p->prop.length == 0)
  {
    parser_raise_error (context_p, PARSER_ERR_PROPERTY_IDENTIFIER_EXPECTED);
  }

  module_node_p->script_path.length = (prop_length_t) (context_p->lit_object.literal_p->prop.length + 1);
  module_node_p->script_path.value_p = (uint8_t *) parser_malloc (context_p,
                                                                  module_node_p->script_path.length * sizeof (uint8_t));

  memcpy (module_node_p->script_path.value_p,
          context_p->lit_object.literal_p->u.char_p,
          module_node_p->script_path.length);
  module_node_p->script_path.value_p[module_node_p->script_path.length - 1] = '\0';

  lexer_next_token (context_p);
} /* parser_module_handle_from_clause */

void
parser_module_set_redirection (parser_context_t *context_p, /**< parser context */
                               bool is_redirected) /**< is redirected */
{
  parser_module_node_t *module_node_p = context_p->module_current_node_p;
  parser_module_names_t *export_name_p = module_node_p->module_names_p;

  for (uint16_t i = 0; i < module_node_p->module_request_count; ++i)
  {
    export_name_p->is_redirected_item = is_redirected;
    export_name_p = export_name_p->next_p;
  }
} /* parser_module_set_redirection */

/**
 * Sets that the parser is parsing the default import / export.
 * Raises parser error if there was a default item before.
 */
void
parser_module_set_default (parser_context_t *context_p) /**< parser context */
{
  if (context_p->module_processing_default_item)
  {
    parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
  }
  context_p->module_processing_default_item = true;
} /* parser_module_set_default */
#endif /* !CONFIG_DISABLE_ES2015_MODULE_SYSTEM */
