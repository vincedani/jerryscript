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

#ifndef CONFIG_DISABLE_ES2015_MODULE_SYSTEM
#define MAX_IMPORT_COUNT 65535
/**
 * Check duplicates in module node.
 * @return true - if the given item is duplicated entry in the current node
 *         false - otherwise
 */
static bool
parser_module_check_for_duplicates_in_node (parser_module_node_t *module_node_p, /**< module node */
                                            const uint8_t *import_name_p, /**< newly imported name */
                                            prop_length_t import_name_length) /**< import name length */
{
  JERRY_ASSERT (import_name_p != NULL);

  parser_module_names_t *import_names_p = module_node_p->module_names_p;

  while (import_names_p != NULL)
  {
    if (import_names_p->import_name_p != NULL)
    {
      uint8_t *current_p = import_names_p->import_name_p;
      prop_length_t current_length = import_names_p->import_name_length;

      if (current_p != NULL && current_length == import_name_length)
      {
        if (memcmp (current_p, import_name_p, current_length) == 0)
        {
          return true;
        }
      }
    }
    import_names_p = import_names_p->next_p;
  }
  return false;
} /* parser_module_check_for_duplicates_in_node */

/**
 * Check duplicates in whole module context.
 * @return true - if the given item is duplicated entry
 *         false - otherwise
 */
static bool
parser_module_check_for_duplicates (parser_context_t *context_p, /**< parser context */
                                    parser_module_node_t *module_node_p, /**< module noda */
                                    lexer_literal_t *import_name_p) /**< newly imported name */
{
  if (import_name_p == NULL)
  {
    return false;
  }

  bool hasDuplicate = parser_module_check_for_duplicates_in_node (module_node_p,
                                                                  import_name_p->u.char_p,
                                                                  import_name_p->prop.length);

  if (!hasDuplicate)
  {
    parser_module_node_t *node_p = context_p->module_context_p->imports_p;
    while (node_p != NULL && !hasDuplicate)
    {
      hasDuplicate = parser_module_check_for_duplicates_in_node (node_p,
                                                                 import_name_p->u.char_p,
                                                                 import_name_p->prop.length);
      node_p = node_p->next_p;
    }
  }

  return hasDuplicate;
} /* parser_module_check_for_duplicates */

/**
 * Delete the saved names from the given module node.
 */
void
parser_module_free_saved_names (parser_module_node_t *module_node_p, bool is_forced_delete) /**< module node */
{
  JERRY_ASSERT (module_node_p != NULL);

  if (module_node_p->module_names_p == NULL)
  {
    return;
  }

  parser_module_names_t *current_p = module_node_p->module_names_p;

  for (uint16_t i = 0; i < module_node_p->module_request_count; i++)
  {
    parser_module_names_t *next_p = current_p->next_p;
    if (!current_p->is_redirected_item || is_forced_delete)
    {
      if (current_p->import_name_p != NULL)
      {
        parser_free (current_p->import_name_p, current_p->import_name_length * sizeof (uint8_t));
        current_p->import_name_p = NULL;
      }

      if (current_p->local_name_p != NULL)
      {
        parser_free (current_p->local_name_p, current_p->local_name_length * sizeof (uint8_t));
        current_p->local_name_p = NULL;
      }
      parser_free (current_p, sizeof (parser_module_names_t));
    }
    current_p = next_p;
  }
} /* parser_module_free_saved_names */

/**
 * Cleanup module node in case of parser error.
 */
void
parser_module_partial_cleanup_on_error (parser_module_node_t *module_node_p) /**< module node */
{
  if (module_node_p != NULL)
  {
    parser_module_free_saved_names (module_node_p, false);

    parser_free (module_node_p, sizeof (parser_module_node_t));
    module_node_p = NULL;
  }
} /* parser_module_partial_cleanup_on_error */

/**
 * Add export node to parser context.
 */
void
parser_module_add_export_node_to_context (parser_context_t *context_p, /**< parser context */
                                          parser_module_node_t *module_node_p) /**< module node */
{
  if (context_p->module_context_p->exports_p != NULL)
  {
    parser_module_names_t *module_names_p = module_node_p->module_names_p;

    for (uint16_t i = 0; i < module_node_p->module_request_count - 1 ; i++)
    {
      module_names_p = module_names_p->next_p;
    }

    module_names_p->next_p = context_p->module_context_p->exports_p->module_names_p;

    context_p->module_context_p->exports_p->module_names_p = module_node_p->module_names_p;
    int request_count =
    context_p->module_context_p->exports_p->module_request_count + module_node_p->module_request_count;

    if (request_count < MAX_IMPORT_COUNT)
    {
      context_p->module_context_p->exports_p->module_request_count = (uint16_t) request_count;
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
parser_module_add_import_node_to_context (parser_context_t *context_p, /**< parser context */
                                          parser_module_node_t *module_node_p) /**< module node */
{
  parser_module_node_t *stored_imports = context_p->module_context_p->imports_p;
  bool is_stored_module = false;

  while (stored_imports != NULL)
  {
    if (stored_imports->script_path_length == module_node_p->script_path_length
        && memcmp (stored_imports->script_path_p, module_node_p->script_path_p, module_node_p->script_path_length) == 0)
    {
      parser_module_names_t *module_names_p = module_node_p->module_names_p;
      is_stored_module = true;

      for (uint16_t i = 0; i < module_node_p->module_request_count - 1; i++)
      {
        module_names_p = module_names_p->next_p;
      }

      module_names_p->next_p = stored_imports->module_names_p;
      stored_imports->module_names_p = module_names_p;

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
                                parser_module_node_t *module_node_p, /**< current module node */
                                lexer_literal_t *import_name_p, /**< import name */
                                lexer_literal_t *local_name_p, /**< local name */
                                bool is_import_item) /**< given item is import item */
{
  if (is_import_item &&
      parser_module_check_for_duplicates (context_p, module_node_p, import_name_p))
  {
    parser_module_free_saved_names (module_node_p, false);
    parser_raise_error (context_p, PARSER_ERR_DUPLICATED_LABEL);
  }

  parser_module_names_t *new_names_p =
  (parser_module_names_t *) parser_malloc (context_p, sizeof (parser_module_names_t));

  new_names_p->next_p = module_node_p->module_names_p;
  module_node_p->module_names_p = new_names_p;

  /* An empty record if the whole module is requested */
  if (import_name_p == NULL)
  {
    module_node_p->module_names_p->import_name_p = NULL;
    module_node_p->module_names_p->import_name_length = 0;
  }
  else
  {
    prop_length_t length = import_name_p->prop.length;
    module_node_p->module_names_p->import_name_length = length;
    module_node_p->module_names_p->import_name_p = (uint8_t *) parser_malloc (context_p, length * sizeof (uint8_t));
    memcpy (module_node_p->module_names_p->import_name_p, import_name_p->u.char_p, length);
  }

  if (local_name_p == NULL)
  {
    module_node_p->module_names_p->local_name_p = NULL;
    module_node_p->module_names_p->local_name_length = 0;
  }
  else
  {
    prop_length_t length = local_name_p->prop.length;
    module_node_p->module_names_p->local_name_length = length;
    module_node_p->module_names_p->local_name_p = (uint8_t *) parser_malloc (context_p, length * sizeof (uint8_t));
    memcpy (module_node_p->module_names_p->local_name_p, local_name_p->u.char_p, length);
  }

  module_node_p->module_request_count++;
} /* parser_module_add_item_to_node */

/**
 * Cleanup the whole module context from parser context.
 */
void
parser_module_cleanup_module_context (parser_context_t *context_p) /**< parser context */
{
  parser_module_context_t *module_context_p = context_p->module_context_p;

  if (module_context_p == NULL)
  {
    return;
  }

  parser_module_node_t *current_node_p = module_context_p->imports_p;

  while (current_node_p != NULL)
  {
    parser_free (current_node_p->script_path_p, current_node_p->script_path_length * sizeof (uint8_t));
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

  if (module_context_p->has_error)
  {
    parser_module_free_saved_names (&module_context_p->cleanup_node, false);
  }

  parser_free (module_context_p, sizeof (parser_module_context_t));
  context_p->module_context_p = NULL;
} /* parser_module_cleanup_module_context */

/**
 * Create module context and bind to the parser context.
 */
void
parser_module_context_init (parser_context_t *context_p) /**< parser context */
{
  if (context_p->module_context_p == NULL)
  {
    context_p->module_context_p =
    (parser_module_context_t *) parser_malloc (context_p, sizeof (parser_module_context_t));

    context_p->module_context_p->exports_p = NULL;
    context_p->module_context_p->imports_p = NULL;
  }
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
  parser_module_node_t *node = (parser_module_node_t *) parser_malloc (context_p, sizeof (parser_module_node_t));

  if (template_node_p != NULL)
  {
    node->module_names_p = template_node_p->module_names_p;
    node->module_request_count = template_node_p->module_request_count;

    node->script_path_p = template_node_p->script_path_p;
    node->script_path_length = template_node_p->script_path_length;
    node->next_p = NULL;
  }
  else
  {
    memset (node, 0, sizeof (parser_module_node_t));
  }

  return node;
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
parser_module_parse_export_item_list (parser_context_t *context_p, /**< parser context */
                                      parser_module_node_t *module_node_p) /**< module node */
{
  if (context_p->token.type == LEXER_LITERAL
      && lexer_compare_raw_identifier_to_current (context_p, "from", 4))
  {
    parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
  }

  if (context_p->token.type == LEXER_KEYW_DEFAULT)
  {
    /* TODO: This part is going to be implemented in the next part of the patch. */
    parser_raise_error (context_p, PARSER_ERR_NOT_IMPLEMENTED);
  }

  bool has_export_name = false;
  lexer_literal_t *export_name_p = NULL;
  lexer_literal_t *local_name_p = NULL;

  while (true)
  {
    bool whole_module_needed = context_p->token.type == LEXER_MULTIPLY;
    if ((!whole_module_needed || has_export_name)
        && context_p->token.type != LEXER_KEYW_DEFAULT
        && (context_p->token.type != LEXER_LITERAL
          || lexer_compare_raw_identifier_to_current (context_p, "from", 4)
          || lexer_compare_raw_identifier_to_current (context_p, "as", 2)))
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
    }

    if (whole_module_needed)
    {
      local_name_p = NULL;
    }
    else
    {
      if (context_p->token.lit_location.type != LEXER_IDENT_LITERAL
          && context_p->token.lit_location.type != LEXER_STRING_LITERAL)
      {
        parser_module_free_saved_names (module_node_p, false);
        parser_raise_error (context_p, PARSER_ERR_PROPERTY_IDENTIFIER_EXPECTED);
      }

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
      parser_module_add_item_to_node (context_p, module_node_p, export_name_p, local_name_p, false);
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
      parser_module_add_item_to_node (context_p, module_node_p, export_name_p, local_name_p, false);
      break;
    }
    lexer_next_token (context_p);
  }
} /* parser_module_parse_export_item_list */

/**
 * Parse import item list.
 */
void
parser_module_parse_import_item_list (parser_context_t *context_p, /**< parser context */
                                      parser_module_node_t *module_node_p) /**< module node */
{
  /* Import list is empty, the whole module will be loaded. */
  if (context_p->token.type == LEXER_LITERAL
      && lexer_compare_raw_identifier_to_current (context_p, "from", 4))
  {
    parser_module_add_item_to_node (context_p, module_node_p, NULL, NULL, true);
    return;
  }

  bool has_import_name = false;
  lexer_literal_t *import_name_p = NULL;
  lexer_literal_t *local_name_p = NULL;

  while (true)
  {
    bool whole_module_needed = context_p->token.type == LEXER_MULTIPLY;
    if ((!whole_module_needed || has_import_name)
        && (context_p->token.type != LEXER_LITERAL
          || lexer_compare_raw_identifier_to_current (context_p, "from", 4)
          || lexer_compare_raw_identifier_to_current (context_p, "as", 2)))
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
    }

    if (whole_module_needed)
    {
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
      parser_module_add_item_to_node (context_p, module_node_p, import_name_p, local_name_p, true);
      break;
    }

    if (context_p->token.type == LEXER_COMMA)
    {
      parser_module_add_item_to_node (context_p, module_node_p, import_name_p, local_name_p, true);
      has_import_name = false;
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

/**
 * Check if property is exported from the script.
 * @returns true - if the given property is exported
 *          false - otherwise
 */
static bool
parser_module_is_property_exported (ecma_string_t *property_name_p, /**< property name */
                                    parser_module_node_t *export_node_p, /**< export node */
                                    parser_module_names_t *export_name_p) /**< [out] export name */
{
  parser_module_names_t *current_p = export_node_p->module_names_p;

  if (current_p == NULL)
  {
    return false;
  }

  for (uint16_t i = 0; i < export_node_p->module_request_count; i++)
  {
    parser_module_names_t *next_p = current_p->next_p;
    ecma_string_t *import_name_p = ecma_new_ecma_string_from_utf8 (current_p->local_name_p,
                                                                   current_p->local_name_length);
    bool found = ecma_compare_ecma_strings (import_name_p, property_name_p);

    ecma_deref_ecma_string (import_name_p);

    if (found)
    {
      *export_name_p = *current_p;
      return true;
    }
    current_p = next_p;
  }

  return false;
} /* parser_module_is_property_exported */

/**
 * Compare property name with imports.
 * @return true - if the exported property is imported
 *         false - otherwise
 */
static bool
parser_module_compare_property_name_with_import (parser_module_node_t *module_node_p, /**< module node */
                                                 parser_module_names_t *export_names_p, /**< export names */
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

    if (current_p->local_name_length == export_names_p->import_name_length)
    {
      if (memcmp (export_names_p->import_name_p, current_p->local_name_p, current_p->local_name_length) == 0)
      {
        *eventual_names_p = *current_p;
        return true;
      }
    }

    current_p = next_p;
  }

  return false;
} /* parser_module_compare_property_name_with_import */

static bool
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

    if (current_p->local_name_p == NULL)
    {
      *eventual_names_p = *current_p;
      return true;
    }

    current_p = next_p;
  }

  return false;
} /* parser_module_is_whole_module_requested */

/**
 * Run an EcmaScript module created by parser_module_parse.
 */
static parser_error_t
parser_module_run (const char *file_path_p, /**< file path */
                   size_t path_size, /**< length of the path */
                   const char *source_p, /**< module source */
                   size_t source_size, /**< length of the source */
                   parser_module_node_t *module_node_p) /**< module node */
{
  parser_module_node_t export_node;

  parser_module_context_t module_context;
  module_context.imports_p = module_node_p;
  module_context.exports_p = &export_node;
  module_context.has_error = false;

  parser_module_context_t *prev_module_context_p = JERRY_CONTEXT (module_top_context_p);
  JERRY_CONTEXT (module_top_context_p) = &module_context;

  jerry_value_t ret_value = jerry_parse ((jerry_char_t *) file_path_p,
                                         path_size,
                                         (jerry_char_t *) source_p,
                                         source_size,
                                         JERRY_PARSE_STRICT_MODE);

  if (jerry_value_is_error (ret_value))
  {
    jerry_release_value (ret_value);
    return PARSER_ERR_MODULE_REQUEST_NOT_FOUND;
  }

  parser_error_t error = PARSER_ERR_NO_ERROR;

  jerry_value_t func_val = ret_value;
  ecma_object_t *func_obj_p = ecma_get_object_from_value (func_val);

  JERRY_ASSERT (ecma_get_object_type (func_obj_p) == ECMA_OBJECT_TYPE_FUNCTION);

  ecma_object_t *scope_p = ecma_create_decl_lex_env (ecma_get_global_environment ());
  ret_value = vm_run_module (ecma_op_function_get_compiled_code ((ecma_extended_object_t *) func_obj_p), scope_p);

  if (jerry_value_is_error (ret_value))
  {
    jerry_release_value (ret_value);
    error = PARSER_ERR_MODULE_REQUEST_NOT_FOUND;
  }

  JERRY_CONTEXT (module_top_context_p) = prev_module_context_p;

  ecma_object_t *global_obj_p = ecma_builtin_get (ECMA_BUILTIN_ID_GLOBAL);
  ecma_property_header_t *module_properties_p = ecma_get_property_list (scope_p);

  parser_module_names_t collective_name;
  memset (&collective_name, 0, sizeof (parser_module_names_t));

  bool is_whole_module_requested = parser_module_is_whole_module_requested (module_node_p, &collective_name);

  ecma_object_t *module_obj_p;

  if (collective_name.import_name_p == NULL)
  {
    module_obj_p = ecma_builtin_get (ECMA_BUILTIN_ID_GLOBAL);
    ecma_ref_object (module_obj_p);
  }
  else
  {
    module_obj_p = ecma_create_object (ecma_builtin_get (ECMA_BUILTIN_ID_OBJECT_PROTOTYPE),
                                       0,
                                       ECMA_OBJECT_TYPE_GENERAL);
  }

  while (module_properties_p != NULL)
  {
    ecma_property_pair_t *prop_pair_p = (ecma_property_pair_t *) module_properties_p;

    for (int i = 0; i < ECMA_PROPERTY_PAIR_ITEM_COUNT; i++)
    {
      ecma_property_t *property_p = (ecma_property_t *) (module_properties_p->types + i);

      if (ECMA_PROPERTY_GET_NAME_TYPE (*property_p) == ECMA_DIRECT_STRING_MAGIC
          && prop_pair_p->names_cp[i] >= LIT_NON_INTERNAL_MAGIC_STRING__COUNT)
      {
        continue;
      }

      ecma_string_t *prop_name_p = ecma_string_from_property_name (*property_p, prop_pair_p->names_cp[i]);

      parser_module_names_t exported_name;
      if (!parser_module_is_property_exported (prop_name_p, &export_node, &exported_name))
      {
        ecma_deref_ecma_string (prop_name_p);
        continue;
      }

      if (is_whole_module_requested)
      {
        ecma_string_t *new_property_name_p = ecma_new_ecma_string_from_utf8 (exported_name.import_name_p,
                                                                             exported_name.import_name_length);

        ecma_property_t *new_property_p;
        ecma_create_named_data_property (module_obj_p,
                                         new_property_name_p,
                                         ECMA_PROPERTY_NOT_WRITABLE,
                                         &new_property_p);

        ecma_named_data_property_assign_value (module_obj_p,
                                              ECMA_PROPERTY_VALUE_PTR (new_property_p),
                                              prop_pair_p->values[i].value);

        ecma_deref_ecma_string (new_property_name_p);
      }

      parser_module_names_t new_name;
      if (parser_module_compare_property_name_with_import (module_node_p, &exported_name, &new_name))
      {
        ecma_string_t *new_property_name_p = ecma_new_ecma_string_from_utf8 (new_name.import_name_p,
                                                                             new_name.import_name_length);

        ecma_property_t *new_property_p;
        ecma_create_named_data_property (global_obj_p,
                                         new_property_name_p,
                                         ECMA_PROPERTY_NOT_WRITABLE,
                                         &new_property_p);

        ecma_named_data_property_assign_value (global_obj_p,
                                              ECMA_PROPERTY_VALUE_PTR (new_property_p),
                                              prop_pair_p->values[i].value);

        ecma_deref_ecma_string (new_property_name_p);
      }

      ecma_deref_ecma_string (prop_name_p);
    }

    module_properties_p = ECMA_GET_POINTER (ecma_property_header_t,
                                            module_properties_p->next_property_cp);
  }

  if (is_whole_module_requested && collective_name.import_name_p != NULL)
  {
    ecma_string_t *collective_name_p = ecma_new_ecma_string_from_utf8 (collective_name.import_name_p,
                                                                       collective_name.import_name_length);
    ecma_property_t *collective_prop_p;
    ecma_create_named_data_property (global_obj_p,
                                     collective_name_p,
                                     ECMA_PROPERTY_NOT_WRITABLE,
                                     &collective_prop_p);

    ecma_named_data_property_assign_value (global_obj_p,
                                           ECMA_PROPERTY_VALUE_PTR (collective_prop_p),
                                           ecma_make_object_value (module_obj_p));
    ecma_deref_ecma_string (collective_name_p);
  }

  ecma_module_add_lex_env (scope_p);
  ecma_deref_object (module_obj_p);
  jerry_release_value (func_val);
  parser_module_free_saved_names (&export_node, false);

  return error;
} /* parser_module_run */

/**
 * Load imported modules.
 */
void
parser_module_load_modules (parser_context_t *context_p) /**< parser context */
{
  parser_module_node_t *current_p = context_p->module_context_p->imports_p;

  while (current_p != NULL)
  {
    uint8_t *script_path = current_p->script_path_p;
    prop_length_t path_length = current_p->script_path_length;

    size_t size = 0;
    uint8_t *buffer_p = jerry_port_module_read_source ((const char *) script_path, &size);

    if (buffer_p == NULL)
    {
      parser_raise_error (context_p, PARSER_ERR_FILE_NOT_FOUND);
    }

    parser_error_t error = parser_module_run ((const char *) script_path,
                                              path_length,
                                              (const char *) buffer_p,
                                              size, current_p);

    jerry_port_module_release_source (buffer_p);

    if (error != PARSER_ERR_NO_ERROR)
    {
      parser_raise_error (context_p, error);
    }

    current_p = current_p->next_p;
  }
} /* parser_module_load_modules */

/**
 * Check if the import statement contains valid aliases.
 */
static void
parser_module_check_valid_aliases (parser_context_t *context_p)
{
  parser_module_node_t *imports_p = context_p->module_context_p->imports_p;
  if (imports_p == NULL)
  {
    return;
  }

  parser_module_names_t collective_name;
  bool is_whole_module_requested = parser_module_is_whole_module_requested (imports_p, &collective_name);

  if (!is_whole_module_requested || (is_whole_module_requested && collective_name.import_name_p != NULL))
  {
    return;
  }

  parser_module_names_t *import_name_p = imports_p->module_names_p;
  for (uint16_t i = 0; i < imports_p->module_request_count; ++i)
  {
    if (import_name_p->local_name_p != NULL
       && (import_name_p->import_name_length == import_name_p->local_name_length)
       && memcmp (import_name_p->local_name_p,
                  import_name_p->local_name_p,
                  import_name_p->local_name_length) == 0)
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_ALIASES);
    }

    import_name_p = import_name_p->next_p;
  }
} /* parser_module_check_valid_aliases */

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
  parser_module_names_t *export_iterator_p;

  for (uint16_t i = 0; i < parent_context_p->imports_p->module_request_count; ++i)
  {
    bool request_is_found_in_module = false;

    /* Whole module is requested, so searching in exports is unnecessary. */
    if (import_name_p->local_name_p == NULL)
    {
      request_is_found_in_module = true;
      break;
    }

    export_iterator_p = current_exports_p;

    for (uint16_t j = 0; j < context_p->module_context_p->exports_p->module_request_count;
         ++j, export_iterator_p = export_iterator_p->next_p)
    {
      if (import_name_p->local_name_length != export_iterator_p->import_name_length)
      {
        continue;
      }

      if (memcmp (import_name_p->local_name_p,
                  export_iterator_p->import_name_p,
                  import_name_p->local_name_length) == 0)
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
parser_module_check_request_place (parser_context_t *context_p)
{
  if (context_p->last_context_p != NULL
      || context_p->stack_top_uint8 != 0
      || vm_is_direct_eval_form_call ())
  {
    parser_raise_error (context_p, PARSER_ERR_MODULE_UNEXPECTED);
  }
} /* parser_module_check_request_place */

void
parser_module_handle_from_clause (parser_context_t *context_p, parser_module_node_t *module_node_p)
{
  /* Store the note temporary in case of the lexer_expect_object_literal_id throws an error. */
  context_p->module_context_p->cleanup_node = *module_node_p;
  context_p->module_context_p->has_error = true;

  lexer_expect_object_literal_id (context_p, LEXER_OBJ_IDENT_NO_OPTS);

  if (context_p->lit_object.literal_p->prop.length == 0)
  {
    parser_raise_error (context_p, PARSER_ERR_PROPERTY_IDENTIFIER_EXPECTED);
  }

  module_node_p->script_path_length = (prop_length_t)(context_p->lit_object.literal_p->prop.length + 1);
  module_node_p->script_path_p =
  (uint8_t *) parser_malloc (context_p, module_node_p->script_path_length * sizeof (uint8_t));

  memcpy (module_node_p->script_path_p,
          context_p->lit_object.literal_p->u.char_p,
          module_node_p->script_path_length);
  module_node_p->script_path_p[module_node_p->script_path_length - 1] = '\0';
  lexer_next_token (context_p);

} /* parser_module_handle_from_clause */

void
parser_module_set_redirection (parser_module_node_t *module_node_p, bool is_redirected)
{
  parser_module_names_t *export_name_p = module_node_p->module_names_p;
  for (uint16_t i = 0; i < module_node_p->module_request_count; ++i)
  {
    export_name_p->is_redirected_item = is_redirected;
    export_name_p = export_name_p->next_p;
  }
} /* parser_module_set_redirection */
#endif /* !CONFIG_DISABLE_ES2015_MODULE_SYSTEM */
