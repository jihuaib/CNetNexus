#ifndef NN_CFG_H
#define NN_CFG_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

// ============================================================================
// CLI MSG Definitions
// ============================================================================
#define NN_CFG_MSG_TYPE_CLI 0x00000001
#define NN_CFG_MSG_TYPE_CLI_RESP 0x00000002
#define NN_CFG_MSG_TYPE_CLI_VIEW_CHG 0x00000003

#define NN_CFG_CLI_MAX_RESP_LEN 4096

// ============================================================================
// CLI VIEW Definitions
// ============================================================================
#define NN_CFG_CLI_VIEW_GLOBAL 0x00000001
#define NN_CFG_CLI_VIEW_USER 0x00000002
#define NN_CFG_CLI_VIEW_CONFIG 0x00000003
#define NN_CFG_CLI_VIEW_BGP 0x00000004

#define NN_CFG_CLI_MAX_VIEW_NAME_LEN 20

#define NN_CFG_CLI_MAX_VIEW_LEN 64
#define NN_CFG_CLI_MAX_PROMPT_LEN 128

// ============================================================================
// TLV Format Definitions
// ============================================================================

// TLV Message Format:
// [Module ID: 4 bytes] [Element1: ID(4) + Len(2) + Value] [Element2: ...] ...

#define NN_CFG_TLV_GROUP_ID_SIZE 4
#define NN_CFG_TLV_ELEMENT_ID_SIZE 4
#define NN_CFG_TLV_LENGTH_SIZE 2
#define NN_CFG_TLV_HEADER_SIZE (NN_CFG_TLV_ELEMENT_ID_SIZE + NN_CFG_TLV_LENGTH_SIZE)

// Forward declaration
typedef struct nn_cli_param_type nn_cli_param_type_t;

// ============================================================================
// TLV Parser Context
// ============================================================================

typedef struct nn_cfg_tlv_parser
{
    const uint8_t *data; // Original data buffer
    uint32_t total_len;  // Total buffer length
    uint32_t offset;     // Current parsing offset
    uint32_t group_id;   // Parsed group ID
} nn_cfg_tlv_parser_t;

// ============================================================================
// TLV Parser Functions
// ============================================================================

/**
 * @brief Initialize TLV parser
 * @param parser Parser context
 * @param data TLV data buffer
 * @param len Buffer length
 * @return 0 on success, -1 on failure
 */
static inline int nn_cfg_tlv_parser_init(nn_cfg_tlv_parser_t *parser, const uint8_t *data, uint32_t len)
{
    if (!parser || !data || len < NN_CFG_TLV_GROUP_ID_SIZE)
    {
        return -1;
    }

    parser->data = data;
    parser->total_len = len;
    parser->offset = 0;

    // Parse module ID (first 4 bytes, network byte order)
    uint32_t group_id_be;
    memcpy(&group_id_be, data, NN_CFG_TLV_GROUP_ID_SIZE);
    parser->group_id = ntohl(group_id_be);
    parser->offset = NN_CFG_TLV_GROUP_ID_SIZE;

    return 0;
}

/**
 * @brief Get next TLV element
 * @param parser Parser context
 * @param out_id Output element ID
 * @param out_value Output value pointer (points into original buffer)
 * @param out_len Output value length
 * @return 1 if element found, 0 if no more elements, -1 on error
 */
static inline int nn_cfg_tlv_parser_next(nn_cfg_tlv_parser_t *parser, uint32_t *out_id, const uint8_t **out_value,
                                         uint16_t *out_len)
{
    if (!parser || !out_id || !out_value || !out_len)
    {
        return -1;
    }

    // Check if we have enough data for TLV header
    if (parser->offset + NN_CFG_TLV_HEADER_SIZE > parser->total_len)
    {
        return 0; // No more elements
    }

    // Parse element ID (4 bytes, network byte order)
    uint32_t elem_id_be;
    memcpy(&elem_id_be, parser->data + parser->offset, NN_CFG_TLV_ELEMENT_ID_SIZE);
    *out_id = ntohl(elem_id_be);
    parser->offset += NN_CFG_TLV_ELEMENT_ID_SIZE;

    // Parse length (2 bytes, network byte order)
    uint16_t len_be;
    memcpy(&len_be, parser->data + parser->offset, NN_CFG_TLV_LENGTH_SIZE);
    *out_len = ntohs(len_be);
    parser->offset += NN_CFG_TLV_LENGTH_SIZE;

    // Check if we have enough data for value
    if (parser->offset + *out_len > parser->total_len)
    {
        return -1; // Malformed TLV
    }

    // Set value pointer (points into original buffer)
    *out_value = (*out_len > 0) ? (parser->data + parser->offset) : NULL;
    parser->offset += *out_len;

    return 1;
}

// ============================================================================
// Convenience Macros for TLV Parsing
// ============================================================================

/**
 * @brief Parse TLV message and iterate through elements
 * Usage:
 *   NN_CFG_TLV_PARSE_BEGIN(msg->data, msg->data_len, parser, group_id) {
 *       NN_CFG_TLV_FOREACH(parser, elem_id, value, len) {
 *           // Process element
 *       }
 *   } NN_CFG_TLV_PARSE_END()
 */
#define NN_CFG_TLV_PARSE_BEGIN(_data, _len, _parser_var, _group_id_var)                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        nn_cfg_tlv_parser_t _parser_var;                                                                               \
        if (nn_cfg_tlv_parser_init(&(_parser_var), (const uint8_t *)(_data), (_len)) == 0)                             \
        {                                                                                                              \
            uint32_t _group_id_var = (_parser_var).group_id;

#define NN_CFG_TLV_PARSE_END()                                                                                         \
    }                                                                                                                  \
    }                                                                                                                  \
    while (0)

/**
 * @brief Iterate through all TLV elements
 */
#define NN_CFG_TLV_FOREACH(_parser_var, _id_var, _value_var, _len_var)                                                 \
    uint32_t _id_var;                                                                                                  \
    const uint8_t *_value_var;                                                                                         \
    uint16_t _len_var;                                                                                                 \
    int _tlv_ret_##parser_var;                                                                                         \
    while ((_tlv_ret_##parser_var = nn_cfg_tlv_parser_next(&(_parser_var), &(_id_var), &(_value_var), &_len_var)) == 1)

/**
 * @brief Extract string value from TLV element
 */
#define NN_CFG_TLV_GET_STRING(_value_ptr, _len, _out_str, _max_len)                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        uint32_t _copy_len = ((_len) < (_max_len) - 1) ? (_len) : ((_max_len) - 1);                                    \
        if ((_value_ptr) && (_copy_len) > 0)                                                                           \
        {                                                                                                              \
            memcpy((_out_str), (_value_ptr), (_copy_len));                                                             \
            (_out_str)[(_copy_len)] = '\0';                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_str)[0] = '\0';                                                                                      \
        }                                                                                                              \
    } while (0)

/**
 * @brief Extract uint32_t value from TLV element (network byte order)
 */
#define NN_CFG_TLV_GET_UINT32(_value_ptr, _len, _out_val)                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == sizeof(uint32_t))                                                                \
        {                                                                                                              \
            uint32_t _val_be;                                                                                          \
            memcpy(&(_val_be), (_value_ptr), sizeof(uint32_t));                                                        \
            (_out_val) = ntohl(_val_be);                                                                               \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_val) = 0;                                                                                            \
        }                                                                                                              \
    } while (0)

/**
 * @brief Extract uint16_t value from TLV element (network byte order)
 */
#define NN_CFG_TLV_GET_UINT16(_value_ptr, _len, _out_val)                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == sizeof(uint16_t))                                                                \
        {                                                                                                              \
            uint16_t _val_be;                                                                                          \
            memcpy(&(_val_be), (_value_ptr), sizeof(uint16_t));                                                        \
            (_out_val) = ntohs(_val_be);                                                                               \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_val) = 0;                                                                                            \
        }                                                                                                              \
    } while (0)

/**
 * @brief Extract uint8_t value from TLV element
 */
#define NN_CFG_TLV_GET_UINT8(_value_ptr, _len, _out_val)                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == sizeof(uint8_t))                                                                 \
        {                                                                                                              \
            (_out_val) = *(_value_ptr);                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (_out_val) = 0;                                                                                            \
        }                                                                                                              \
    } while (0)

/**
 * @brief Extract IPv4 address from TLV element
 */
#define NN_CFG_TLV_GET_IPV4(_value_ptr, _len, _out_str, _max_len)                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((_value_ptr) && (_len) == 4)                                                                               \
        {                                                                                                              \
            snprintf((_out_str), (_max_len), "%u.%u.%u.%u", (_value_ptr)[0], (_value_ptr)[1], (_value_ptr)[2],         \
                     (_value_ptr)[3]);                                                                                 \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            (out_str)[0] = '\0';                                                                                       \
        }                                                                                                              \
    } while (0)


// ============================================================================
// PUBLIC API
// ============================================================================

// Register a module's XML configuration path by module ID
// This should be called by modules in their constructor after nn_dev_register_module
void nn_cfg_register_module_xml(uint32_t module_id, const char *xml_path);

// Get view prompt template by view name (for modules to fill placeholders)
int nn_cfg_get_view_prompt_template(uint32_t view_id, char *view_name);

/**
 * Parse a type string like "string(1-63)" or "uint(0-65535)" into a param type structure
 * @param type_str The type string to parse
 * @return Newly allocated param type structure, or NULL on error
 */
nn_cli_param_type_t *nn_cfg_param_type_parse(const char *type_str);

void nn_cfg_param_type_free(nn_cli_param_type_t *param_type);

gboolean nn_cfg_param_type_validate(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                                    uint32_t error_msg_size);

#endif // NN_CFG_H
