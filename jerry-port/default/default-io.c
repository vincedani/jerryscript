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

#include <stdarg.h>
#include <stdlib.h>

#include "jerryscript-port.h"
#include "jerryscript-port-default.h"
#include "jerryscript-debugger.h"

#ifndef DISABLE_EXTRA_API

/**
 * Actual log level
 */
static jerry_log_level_t jerry_port_default_log_level = JERRY_LOG_LEVEL_ERROR;

#define JERRY_PORT_DEFAULT_LOG_LEVEL jerry_port_default_log_level

/**
 * Get the log level
 *
 * @return current log level
 *
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled without the DISABLE_EXTRA_API macro.
 */
jerry_log_level_t
jerry_port_default_get_log_level (void)
{
  return jerry_port_default_log_level;
} /* jerry_port_default_get_log_level */

/**
 * Set the log level
 *
 * Note:
 *      This function is only available if the port implementation library is
 *      compiled without the DISABLE_EXTRA_API macro.
 */
void
jerry_port_default_set_log_level (jerry_log_level_t level) /**< log level */
{
  jerry_port_default_log_level = level;
} /* jerry_port_default_set_log_level */

#else /* DISABLE_EXTRA_API */
#define JERRY_PORT_DEFAULT_LOG_LEVEL JERRY_LOG_LEVEL_ERROR
#endif /* !DISABLE_EXTRA_API */

/**
 * Default implementation of jerry_port_log. Prints log message to the standard
 * error with 'vfprintf' if message log level is less than or equal to the
 * current log level.
 *
 * If debugger support is enabled, printing happens first to an in-memory buffer,
 * which is then sent both to the standard error and to the debugger client.
 *
 * Note:
 *      Changing the log level from JERRY_LOG_LEVEL_ERROR is only possible if
 *      the port implementation library is compiled without the
 *      DISABLE_EXTRA_API macro.
 */
void
jerry_port_log (jerry_log_level_t level, /**< message log level */
                const char *format, /**< format string */
                ...)  /**< parameters */
{
  if (level <= JERRY_PORT_DEFAULT_LOG_LEVEL)
  {
    va_list args;
    va_start (args, format);
#ifdef JERRY_DEBUGGER
    int length = vsnprintf (NULL, 0, format, args);
    va_end (args);
    va_start (args, format);

    JERRY_VLA (char, buffer, length + 1);
    vsnprintf (buffer, (size_t) length + 1, format, args);

    fprintf (stderr, "%s", buffer);
    jerry_debugger_send_log (level, (jerry_char_t *) buffer, (jerry_size_t) length);
#else /* If jerry-debugger isn't defined, libc is turned on */
    vfprintf (stderr, format, args);
#endif /* JERRY_DEBUGGER */
    va_end (args);
  }
} /* jerry_port_log */

#ifndef CONFIG_DISABLE_ES2015_MODULE_SYSTEM

/**
 * Determines the size of the given file.
 * @return size of the file
 */
static size_t
jerry_port_get_file_size (FILE *file_p) /**< opened file */
{
  fseek (file_p, 0, SEEK_END);
  long size = ftell (file_p);
  fseek (file_p, 0, SEEK_SET);

  return (size_t) size;
} /* jerry_port_get_file_size */

/**
 * Opens file with the given path and reads its source.
 * @return the source of the file
 */
uint8_t *
jerry_port_module_read_source (const char *file_name_p, /**< file name */
                               size_t *out_size_p) /**< read bytes */
{
  FILE *file_p = fopen (file_name_p, "rb");

  if (file_p == NULL)
  {
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: failed to open file: %s\n", file_name_p);
    return NULL;
  }

  size_t file_size = jerry_port_get_file_size (file_p);
  uint8_t *buffer_p = malloc (file_size);

  if (buffer_p == NULL)
  {
    fclose (file_p);

    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: failed to allocate memory for module");
    return NULL;
  }

  size_t bytes_read = fread (buffer_p, 1u, file_size, file_p);

  if (!bytes_read)
  {
    fclose (file_p);
    jerry_port_module_release_source (buffer_p);

    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Error: failed to read file: %s\n", file_name_p);
    return NULL;
  }

  fclose (file_p);
  *out_size_p = bytes_read;

  return buffer_p;
} /* jerry_port_module_read_source */

/**
 * Release the previously opened file's content.
 */
void
jerry_port_module_release_source (uint8_t *buffer_p) /**< buffer to free */
{
  if (buffer_p == NULL)
  {
    return;
  }

  free (buffer_p);
} /* jerry_port_module_release_source */

#endif /* !CONFIG_DISABLE_ES2015_MODULE_SYSTEM */
