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

#include <unistd.h>
#include <stdlib.h>

#include "jerryscript.h"
#include "jerryscript-ext/module.h"
#include "jerryscript-ext/handler.h"
#include "jrt.h"

static bool
load_and_evaluate_js_file (const jerry_value_t name, jerry_value_t *result)
{
  jerry_size_t name_size = jerry_get_utf8_string_size (name);
  jerry_char_t name_string[name_size + 1];
  jerry_string_to_utf8_char_buffer (name, name_string, name_size);
  name_string[name_size] = 0;

  FILE *js_file = fopen ((const char *) name_string, "r");

  if (!js_file)
  {
    return false;
  }

  fseek (js_file, 0, SEEK_END);
  size_t file_size = (size_t) ftell (js_file);
  JERRY_ASSERT (file_size != 0);
  fseek (js_file, 0, SEEK_SET);

  char *js_file_contents = (char *) malloc (file_size);

  if (js_file_contents == NULL)
  {
    fclose (js_file);
    return false;
  }

  JERRY_UNUSED (fread (js_file_contents, file_size, 1, js_file));
  JERRY_ASSERT (!ferror (js_file));
  fclose (js_file);

  const jerry_char_t *func_args = (const jerry_char_t *) "exports";
  jerry_value_t module_function = jerry_parse_function (name_string,
                          name_size,
                          func_args,
                          strlen ((const char *) func_args),
                          (const jerry_char_t *) js_file_contents,
                          file_size,
                          JERRY_PARSE_NO_OPTS);

  jerry_value_t exports_object = jerry_create_object ();
  jerry_value_t call_result = jerry_call_function (module_function,
                                                   jerry_create_undefined (),
                                                   &exports_object, 1);
  JERRY_ASSERT (!jerry_value_is_error (call_result));

  (*result) = exports_object;

  jerry_release_value (call_result);
  jerry_release_value (module_function);
  free (js_file_contents);

  return true;
} /* load_and_evaluate_js_file */

/**
 * The resovler searches the module in the following folders with the given
 * order:
 *  1. cwd/jerry_modules
 *  2. ~/jerry_modules
 *
 *  cwd: Current working directory
 */
static jerry_value_t
canonicalize_file_path (const jerry_value_t name)
{
  jerry_size_t name_size = jerry_get_utf8_string_size (name);
  jerry_char_t name_string[name_size + 1];
  jerry_string_to_utf8_char_buffer (name, name_string, name_size);
  name_string[name_size] = 0;

  jerry_char_t first_character = name_string[0];

  if (first_character == '/' || first_character == '.')
  {
    return jerry_create_string (name_string);
  }

  // cwd/jerry_modules/...
  char cwd[256] = { 0 };
  char tmp_path[256] = { 0 };

  getcwd (cwd, sizeof (cwd));

  snprintf (tmp_path, sizeof (tmp_path),
            "%s/jerry_modules/%s.js", cwd, (const char *) name_string);

  if (access ((const char *) tmp_path, R_OK) == 0)
  {
    return jerry_create_string_from_utf8 ((const jerry_char_t *) tmp_path);
  }

  // ~/jerry_modules/...
  memset (tmp_path, 0, sizeof (tmp_path));
  char *user_name = getlogin ();

  snprintf (tmp_path, sizeof (tmp_path), "/home/%s/jerry_modules/%s.js",
            user_name, (const char *) name_string);

  if (access ((const char *) tmp_path, R_OK) == 0)
  {
    return jerry_create_string_from_utf8 ((const jerry_char_t *) tmp_path);
  }

  return jerry_create_undefined ();
} /* canonicalize_file_path */

/**
 * Define a resolver for modules which are given with their names or their paths
 * (relative or absolute).
 * Examples:
 *  absolute: require('/home/jerry/scripts/myScript.js');
 *  relative: require('../scripts/myScript.js');
 *            require('~/scripts/myScript.js');
 *  search:   require('myScript');
 */
jerryx_module_resolver_t js_file_loader =
{
  canonicalize_file_path,
  load_and_evaluate_js_file
};
