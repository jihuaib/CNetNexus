#include "nn_cli_param_type.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper function to extract range from parentheses
// Input: "1-63" -> min=1, max=63
// Returns true on success
static bool parse_range(const char *range_str, int64_t *min_val, int64_t *max_val)
{
    if (!range_str || !min_val || !max_val)
    {
        return false;
    }

    char *dash = strchr(range_str, '-');
    if (!dash)
    {
        // Single value, use as both min and max
        char *endptr;
        errno = 0;
        *min_val = strtoll(range_str, &endptr, 10);
        if (errno != 0 || *endptr != '\0')
        {
            return false;
        }
        *max_val = *min_val;
        return true;
    }

    // Parse min value
    char min_str[64] = {0};
    size_t min_len = dash - range_str;
    if (min_len >= sizeof(min_str))
    {
        return false;
    }
    strncpy(min_str, range_str, min_len);

    char *endptr;
    errno = 0;
    *min_val = strtoll(min_str, &endptr, 10);
    if (errno != 0 || *endptr != '\0')
    {
        return false;
    }

    // Parse max value
    errno = 0;
    *max_val = strtoll(dash + 1, &endptr, 10);
    if (errno != 0 || *endptr != '\0')
    {
        return false;
    }

    return true;
}

// Parse type string like "string(1-63)" or "uint(0-65535)"
nn_cli_param_type_t *nn_cli_param_type_parse(const char *type_str)
{
    if (!type_str || *type_str == '\0')
    {
        return NULL;
    }

    nn_cli_param_type_t *param_type = g_malloc0(sizeof(nn_cli_param_type_t));
    param_type->type_str = g_strdup(type_str);

    // Find opening parenthesis
    char *paren_open = strchr(type_str, '(');
    char *paren_close = paren_open ? strchr(paren_open, ')') : NULL;

    // Extract type name
    char type_name[64] = {0};
    size_t name_len = paren_open ? (size_t)(paren_open - type_str) : strlen(type_str);
    if (name_len >= sizeof(type_name))
    {
        name_len = sizeof(type_name) - 1;
    }
    strncpy(type_name, type_str, name_len);

    // Convert to lowercase for comparison
    for (char *p = type_name; *p; p++)
    {
        *p = tolower(*p);
    }

    // Extract range string if present
    char range_str[64] = {0};
    if (paren_open && paren_close && paren_close > paren_open + 1)
    {
        size_t range_len = paren_close - paren_open - 1;
        if (range_len >= sizeof(range_str))
        {
            range_len = sizeof(range_str) - 1;
        }
        strncpy(range_str, paren_open + 1, range_len);
    }

    // Determine type and set validation callback
    if (strcmp(type_name, "string") == 0)
    {
        param_type->type = NN_PARAM_TYPE_STRING;
        param_type->validate = nn_param_validate_string;

        // Parse string length range
        int64_t min_val = 0, max_val = 255;
        if (range_str[0] != '\0')
        {
            parse_range(range_str, &min_val, &max_val);
        }
        param_type->range.string_range.min_len = (uint32_t)min_val;
        param_type->range.string_range.max_len = (uint32_t)max_val;
    }
    else if (strcmp(type_name, "uint") == 0)
    {
        param_type->type = NN_PARAM_TYPE_UINT;
        param_type->validate = nn_param_validate_uint;

        // Parse unsigned integer range
        int64_t min_val = 0, max_val = UINT32_MAX;
        if (range_str[0] != '\0')
        {
            parse_range(range_str, &min_val, &max_val);
        }
        param_type->range.uint_range.min_val = (uint64_t)min_val;
        param_type->range.uint_range.max_val = (uint64_t)max_val;
    }
    else if (strcmp(type_name, "int") == 0)
    {
        param_type->type = NN_PARAM_TYPE_INT;
        param_type->validate = nn_param_validate_int;

        // Parse signed integer range
        int64_t min_val = INT32_MIN, max_val = INT32_MAX;
        if (range_str[0] != '\0')
        {
            parse_range(range_str, &min_val, &max_val);
        }
        param_type->range.int_range.min_val = min_val;
        param_type->range.int_range.max_val = max_val;
    }
    else if (strcmp(type_name, "ipv4") == 0)
    {
        param_type->type = NN_PARAM_TYPE_IPV4;
        param_type->validate = nn_param_validate_ipv4;
    }
    else if (strcmp(type_name, "ipv6") == 0)
    {
        param_type->type = NN_PARAM_TYPE_IPV6;
        param_type->validate = nn_param_validate_ipv6;
    }
    else if (strcmp(type_name, "ip") == 0)
    {
        param_type->type = NN_PARAM_TYPE_IP;
        param_type->validate = nn_param_validate_ip;
    }
    else if (strcmp(type_name, "mac") == 0)
    {
        param_type->type = NN_PARAM_TYPE_MAC;
        param_type->validate = nn_param_validate_mac;
    }
    else
    {
        param_type->type = NN_PARAM_TYPE_UNKNOWN;
        param_type->validate = NULL;
    }

    return param_type;
}

// Validate a parameter value
bool nn_cli_param_type_validate(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                                uint32_t error_msg_size)
{
    if (!param_type || !value)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Invalid parameter or value");
        }
        return false;
    }

    // If no validation callback, accept any value
    if (!param_type->validate)
    {
        return true;
    }

    return param_type->validate(param_type, value, error_msg, error_msg_size);
}

// Get type description
const char *nn_cli_param_type_get_desc(const nn_cli_param_type_t *param_type)
{
    if (!param_type)
    {
        return "unknown";
    }

    switch (param_type->type)
    {
    case NN_PARAM_TYPE_STRING:
        return "string";
    case NN_PARAM_TYPE_UINT:
        return "unsigned integer";
    case NN_PARAM_TYPE_INT:
        return "integer";
    case NN_PARAM_TYPE_IPV4:
        return "IPv4 address";
    case NN_PARAM_TYPE_IPV6:
        return "IPv6 address";
    case NN_PARAM_TYPE_IP:
        return "IP address";
    case NN_PARAM_TYPE_MAC:
        return "MAC address";
    case NN_PARAM_TYPE_ENUM:
        return "enumeration";
    default:
        return "unknown";
    }
}

// Free parameter type
void nn_cli_param_type_free(nn_cli_param_type_t *param_type)
{
    if (!param_type)
    {
        return;
    }

    g_free(param_type->type_str);
    g_free(param_type);
}

// String validation
bool nn_param_validate_string(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                              uint32_t error_msg_size)
{
    if (!param_type || !value)
    {
        return false;
    }

    size_t len = strlen(value);

    if (len < param_type->range.string_range.min_len)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "String too short: minimum %u characters required",
                     param_type->range.string_range.min_len);
        }
        return false;
    }

    if (len > param_type->range.string_range.max_len)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "String too long: maximum %u characters allowed",
                     param_type->range.string_range.max_len);
        }
        return false;
    }

    return true;
}

// Unsigned integer validation
bool nn_param_validate_uint(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                            uint32_t error_msg_size)
{
    if (!param_type || !value)
    {
        return false;
    }

    // Check for valid unsigned integer format
    const char *p = value;
    if (*p == '\0')
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Empty value");
        }
        return false;
    }

    while (*p)
    {
        if (!isdigit(*p))
        {
            if (error_msg && error_msg_size > 0)
            {
                snprintf(error_msg, error_msg_size, "Invalid unsigned integer format");
            }
            return false;
        }
        p++;
    }

    // Parse value
    char *endptr;
    errno = 0;
    unsigned long long val = strtoull(value, &endptr, 10);

    if (errno == ERANGE || *endptr != '\0')
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Value out of range");
        }
        return false;
    }

    // Check range
    if (val < param_type->range.uint_range.min_val || val > param_type->range.uint_range.max_val)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Value must be between %lu and %lu",
                     (unsigned long)param_type->range.uint_range.min_val,
                     (unsigned long)param_type->range.uint_range.max_val);
        }
        return false;
    }

    return true;
}

// Signed integer validation
bool nn_param_validate_int(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                           uint32_t error_msg_size)
{
    if (!param_type || !value)
    {
        return false;
    }

    // Check for valid integer format
    const char *p = value;
    if (*p == '\0')
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Empty value");
        }
        return false;
    }

    // Allow leading minus sign
    if (*p == '-')
    {
        p++;
    }

    if (*p == '\0')
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Invalid integer format");
        }
        return false;
    }

    while (*p)
    {
        if (!isdigit(*p))
        {
            if (error_msg && error_msg_size > 0)
            {
                snprintf(error_msg, error_msg_size, "Invalid integer format");
            }
            return false;
        }
        p++;
    }

    // Parse value
    char *endptr;
    errno = 0;
    long long val = strtoll(value, &endptr, 10);

    if (errno == ERANGE || *endptr != '\0')
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Value out of range");
        }
        return false;
    }

    // Check range
    if (val < param_type->range.int_range.min_val || val > param_type->range.int_range.max_val)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Value must be between %ld and %ld",
                     (long)param_type->range.int_range.min_val, (long)param_type->range.int_range.max_val);
        }
        return false;
    }

    return true;
}

// IPv4 address validation
bool nn_param_validate_ipv4(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                            uint32_t error_msg_size)
{
    (void)param_type; // Unused for IP validation

    if (!value)
    {
        return false;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, value, &addr) != 1)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Invalid IPv4 address format");
        }
        return false;
    }

    return true;
}

// IPv6 address validation
bool nn_param_validate_ipv6(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                            uint32_t error_msg_size)
{
    (void)param_type; // Unused for IP validation

    if (!value)
    {
        return false;
    }

    struct in6_addr addr;
    if (inet_pton(AF_INET6, value, &addr) != 1)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Invalid IPv6 address format");
        }
        return false;
    }

    return true;
}

// IP address validation (IPv4 or IPv6)
bool nn_param_validate_ip(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                          uint32_t error_msg_size)
{
    if (!value)
    {
        return false;
    }

    // Try IPv4 first
    if (nn_param_validate_ipv4(param_type, value, NULL, 0))
    {
        return true;
    }

    // Try IPv6
    if (nn_param_validate_ipv6(param_type, value, NULL, 0))
    {
        return true;
    }

    if (error_msg && error_msg_size > 0)
    {
        snprintf(error_msg, error_msg_size, "Invalid IP address format (IPv4 or IPv6 expected)");
    }
    return false;
}

// MAC address validation
bool nn_param_validate_mac(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                           uint32_t error_msg_size)
{
    (void)param_type; // Unused

    if (!value)
    {
        return false;
    }

    // Accept formats: XX:XX:XX:XX:XX:XX or XX-XX-XX-XX-XX-XX
    int octets[6];
    char sep1, sep2, sep3, sep4, sep5;

    // Try colon format
    if (sscanf(value, "%x%c%x%c%x%c%x%c%x%c%x", &octets[0], &sep1, &octets[1], &sep2, &octets[2], &sep3, &octets[3],
               &sep4, &octets[4], &sep5, &octets[5]) == 11)
    {
        // Check separators are consistent
        if ((sep1 == ':' && sep2 == ':' && sep3 == ':' && sep4 == ':' && sep5 == ':') ||
            (sep1 == '-' && sep2 == '-' && sep3 == '-' && sep4 == '-' && sep5 == '-'))
        {
            // Check octet values
            for (int i = 0; i < 6; i++)
            {
                if (octets[i] < 0 || octets[i] > 255)
                {
                    if (error_msg && error_msg_size > 0)
                    {
                        snprintf(error_msg, error_msg_size, "Invalid MAC address: octet out of range");
                    }
                    return false;
                }
            }
            return true;
        }
    }

    if (error_msg && error_msg_size > 0)
    {
        snprintf(error_msg, error_msg_size, "Invalid MAC address format (expected XX:XX:XX:XX:XX:XX)");
    }
    return false;
}
