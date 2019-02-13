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

#include "module.h"
#include "jerryscript.h"
#include "jcontext.h"

#include "ecma-helpers.h"
#include "ecma-function-object.h"
#include "ecma-literal-storage.h"
#include "ecma-lex-env.h"
#include "ecma-gc.h"
#include "vm.h"

#ifndef CONFIG_DISABLE_ES2015_MODULE_SYSTEM

/**
 * Check if property is exported from the script.
 * @returns the export name - if the given property is exported
 *          NULL - otherwise
 */
static parser_module_names_t *
parser_module_is_property_exported (ecma_string_t *property_name_p, /**< property name */
                                    parser_module_node_t *export_node_p) /**< export node */
{
  parser_module_names_t *current_p = export_node_p->module_names_p;

  if (current_p == NULL)
  {
    return NULL;
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
      return current_p;
    }
    current_p = next_p;
  }

  return NULL;
} /* parser_module_is_property_exported */

/**
 * Compare property name with imports.
 * @return the export name - if the exported property is imported
 *         NULL - otherwise
 */
static parser_module_names_t *
parser_module_compare_property_name_with_import (parser_module_node_t *module_node_p, /**< module node */
                                                 parser_module_names_t *export_names_p) /**< export names */
{
  parser_module_names_t *current_p = module_node_p->module_names_p;

  if (current_p == NULL)
  {
    return NULL;
  }

  for (uint16_t i = 0; i < module_node_p->module_request_count; i++)
  {
    parser_module_names_t *next_p = current_p->next_p;

    if (current_p->local_name_length == export_names_p->import_name_length)
    {
      if (memcmp (export_names_p->import_name_p, current_p->local_name_p, current_p->local_name_length) == 0)
      {
        return current_p;
      }
    }

    current_p = next_p;
  }

  return NULL;
} /* parser_module_compare_property_name_with_import */

/**
 * Connect the imported script's propeties to the main script.
 */
static void
module_connect_properties (ecma_object_t *scope_p) /** scope */
{
  parser_module_context_t *module_context_p = JERRY_CONTEXT (module_top_context_p);

  if (module_context_p == NULL || module_context_p->exports_p == NULL || module_context_p->imports_p == NULL)
  {
    return;
  }

  ecma_object_t *global_obj_p = ecma_builtin_get (ECMA_BUILTIN_ID_GLOBAL);
  ecma_property_header_t *module_properties_p = ecma_get_property_list (scope_p);

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

      parser_module_names_t *exported_name_p = parser_module_is_property_exported (prop_name_p,
                                                                                   module_context_p->exports_p);

      if (exported_name_p == NULL)
      {
        ecma_deref_ecma_string (prop_name_p);
        continue;
      }

      parser_module_names_t *new_name_p = parser_module_compare_property_name_with_import (module_context_p->imports_p,
                                                                                           exported_name_p);
      if (new_name_p != NULL)
      {
        ecma_property_t *new_property_p;
        ecma_string_t *new_property_name_p = ecma_new_ecma_string_from_utf8 (new_name_p->import_name_p,
                                                                             new_name_p->import_name_length);

        // TODO: Check if the global object is extensible.
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

  ecma_module_add_lex_env (scope_p);
  parser_module_free_saved_names (module_context_p->exports_p);
} /* module_connect_properties */

/**
 * Run an EcmaScript module loaded by module_load_modules.
 */
static parser_error_t
parser_module_run (const char *file_path_p, /**< file path */
                   size_t path_size, /**< length of the path */
                   const char *source_p, /**< module source */
                   size_t source_size, /**< length of the source */
                   parser_module_node_t *module_node_p) /**< module node */
{
  parser_module_node_t export_node;
  memset (&export_node, 0, sizeof (parser_module_node_t));

  parser_module_context_t module_context;
  module_context.imports_p = module_node_p;
  module_context.exports_p = &export_node;

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

  module_connect_properties (scope_p);
  jerry_release_value (func_val);

  JERRY_CONTEXT (module_top_context_p) = prev_module_context_p;
  return error;
} /* parser_module_run */

/**
 * Load imported modules.
 */
void
module_load_modules (parser_context_t *context_p) /**< parser context */
{
  parser_module_node_t *current_p = context_p->module_context_p->imports_p;

  while (current_p != NULL)
  {
    uint8_t *script_path_p = current_p->script_path_p;
    prop_length_t path_length = current_p->script_path_length;

    size_t size = 0;
    uint8_t *buffer_p = jerry_port_module_read_source ((const char *) script_path_p, &size);

    if (buffer_p == NULL)
    {
      parser_raise_error (context_p, PARSER_ERR_FILE_NOT_FOUND);
    }

    parser_error_t error = parser_module_run ((const char *) script_path_p,
                                              path_length,
                                              (const char *) buffer_p,
                                              size,
                                              current_p);

    jerry_port_module_release_source (buffer_p);

    if (error != PARSER_ERR_NO_ERROR)
    {
      parser_raise_error (context_p, error);
    }

    current_p = current_p->next_p;
  }
} /* module_load_modules */

#endif /* !CONFIG_DISABLE_ES2015_MODULE_SYSTEM */
