/* Shim: the attribute macros esp_err.h references; nothing here changes
 * codegen for this build. */
#ifndef SHIM_ESP_COMPILER_H
#define SHIM_ESP_COMPILER_H
#ifndef __attribute_deprecated__
#define __attribute_deprecated__ __attribute__((deprecated))
#endif
#define ESP_COMPILER_LOG_ATTR(tag, level)
#endif
