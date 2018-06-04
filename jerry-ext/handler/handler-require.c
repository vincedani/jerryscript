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

#include "jerryscript.h"
#include "jerryscript-ext/module.h"
#include "jerryscript-ext/handler.h"
#include "jrt.h"

jerryx_module_resolver_t **internal_resolvers;
uint8_t resolvers_count;

void
jerryx_handler_require_set_resolvers (jerryx_module_resolver_t **resolvers,
                                      const uint8_t resolvers_cnt)
{
  JERRY_ASSERT (resolvers != NULL);
  JERRY_ASSERT (resolvers_cnt != 0);

  resolvers_count = resolvers_cnt;
  internal_resolvers = resolvers;
} /* jerryx_handler_require_set_resolvers */

jerry_value_t
jerryx_handler_require (const jerry_value_t func_obj_val,
                const jerry_value_t this_p,
                const jerry_value_t args_p[],
                const jerry_length_t args_cnt)
{
  (void) func_obj_val;
  (void) this_p;
  (void) args_cnt;

  jerry_value_t return_value = 0;

  JERRY_ASSERT (args_cnt == 1);
  JERRY_ASSERT (internal_resolvers != NULL);

  return_value = jerryx_module_resolve (args_p[0],
                      (const jerryx_module_resolver_t **) internal_resolvers,
                      resolvers_count);

  return return_value;
} /* jerryx_handler_require */
