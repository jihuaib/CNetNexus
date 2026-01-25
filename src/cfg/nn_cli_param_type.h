#ifndef NN_CLI_PARAM_TYPE_H
#define NN_CLI_PARAM_TYPE_H

#include <stdbool.h>
#include <stdint.h>

// Parameter data types
typedef enum
{
    NN_PARAM_TYPE_UNKNOWN = 0, // Unknown/unspecified type
    NN_PARAM_TYPE_STRING,      // String type: string(min_len-max_len)
    NN_PARAM_TYPE_UINT,        // Unsigned integer: uint(min-max)
    NN_PARAM_TYPE_INT,         // Signed integer: int(min-max)
    NN_PARAM_TYPE_IPV4,        // IPv4 address
    NN_PARAM_TYPE_IPV6,        // IPv6 address
    NN_PARAM_TYPE_IP,          // IPv4 or IPv6 address
    NN_PARAM_TYPE_MAC,         // MAC address
    NN_PARAM_TYPE_ENUM,        // Enumeration (predefined values)
} nn_param_type_enum_t;

// Forward declaration
typedef struct nn_cli_param_type nn_cli_param_type_t;

// Validation callback function type
// Returns true if value is valid, false otherwise
typedef bool (*nn_param_validate_fn)(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                                     uint32_t error_msg_size);

// Parameter type structure
struct nn_cli_param_type
{
    nn_param_type_enum_t type; // Data type enum

    // Range values (interpretation depends on type)
    union
    {
        struct
        {
            uint32_t min_len; // Minimum string length
            uint32_t max_len; // Maximum string length
        } string_range;

        struct
        {
            int64_t min_val; // Minimum value
            int64_t max_val; // Maximum value
        } int_range;

        struct
        {
            uint64_t min_val; // Minimum value
            uint64_t max_val; // Maximum value
        } uint_range;
    } range;

    char *type_str;                // Original type string (e.g., "string(1-63)")
    nn_param_validate_fn validate; // Validation callback
};

// Function prototypes

/**
 * Parse a type string like "string(1-63)" or "uint(0-65535)" into a param type structure
 * @param type_str The type string to parse
 * @return Newly allocated param type structure, or NULL on error
 */
nn_cli_param_type_t *nn_cli_param_type_parse(const char *type_str);

/**
 * Validate a parameter value against its type definition
 * @param param_type The parameter type definition
 * @param value The value to validate
 * @param error_msg Buffer to store error message on failure
 * @param error_msg_size Size of error message buffer
 * @return true if valid, false otherwise
 */
bool nn_cli_param_type_validate(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                                uint32_t error_msg_size);

/**
 * Get a human-readable description of the parameter type
 * @param param_type The parameter type
 * @return Static string describing the type
 */
const char *nn_cli_param_type_get_desc(const nn_cli_param_type_t *param_type);

/**
 * Get the binary value length for TLV encoding based on parameter type
 * @param param_type The parameter type
 * @param value The string value to convert
 * @return Length in bytes for TLV encoding
 */
uint16_t nn_cli_param_type_get_value_length(const nn_cli_param_type_t *param_type, const char *value);

/**
 * Free a parameter type structure
 * @param param_type The structure to free
 */
void nn_cli_param_type_free(nn_cli_param_type_t *param_type);

// Built-in validation functions
bool nn_param_validate_string(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                              uint32_t error_msg_size);
bool nn_param_validate_uint(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                            uint32_t error_msg_size);
bool nn_param_validate_int(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                           uint32_t error_msg_size);
bool nn_param_validate_ipv4(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                            uint32_t error_msg_size);
bool nn_param_validate_ipv6(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                            uint32_t error_msg_size);
bool nn_param_validate_ip(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                          uint32_t error_msg_size);
bool nn_param_validate_mac(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                           uint32_t error_msg_size);

#endif // NN_CLI_PARAM_TYPE_H
