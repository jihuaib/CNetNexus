#include "nn_cli_handler.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nn_cli_tree.h"
#include "nn_cli_xml_parser.h"

// Global CLI view tree
static nn_cli_view_tree_t g_view_tree = {NULL, NULL};

// Global hostname
static char g_hostname[MAX_HOSTNAME_LEN] = "NetNexus";

// Get current hostname
const char *nn_cli_get_hostname(void)
{
    return g_hostname;
}

// Set new hostname
void nn_cli_set_hostname(const char *hostname)
{
    if (hostname && strlen(hostname) > 0)
    {
        strncpy(g_hostname, hostname, MAX_HOSTNAME_LEN - 1);
        g_hostname[MAX_HOSTNAME_LEN - 1] = '\0';
    }
}

// Send a message to the client
void send_message(uint32_t client_fd, const char *message)
{
    if (message)
    {
        write(client_fd, message, strlen(message));
    }
}

// Update session prompt based on current view
static void update_prompt(nn_cli_session_t *session)
{
    if (!session || !session->current_view)
    {
        return;
    }

    if (session->current_view->prompt_template)
    {
        // Check if template contains {hostname} placeholder
        const char *placeholder = "{hostname}";
        const char *template = session->current_view->prompt_template;
        const char *pos = strstr(template, placeholder);

        if (pos)
        {
            // Replace {hostname} with actual hostname
            char temp[64];
            size_t prefix_len = pos - template;
            size_t placeholder_len = strlen(placeholder);

            // Copy prefix
            strncpy(temp, template, prefix_len);
            temp[prefix_len] = '\0';

            // Append hostname
            strncat(temp, nn_cli_get_hostname(), sizeof(temp) - strlen(temp) - 1);

            // Append suffix
            strncat(temp, pos + placeholder_len, sizeof(temp) - strlen(temp) - 1);

            strncpy(session->prompt, temp, sizeof(session->prompt) - 1);
            session->prompt[sizeof(session->prompt) - 1] = '\0';
        }
        else
        {
            // No placeholder, use template as-is
            strncpy(session->prompt, template, sizeof(session->prompt) - 1);
            session->prompt[sizeof(session->prompt) - 1] = '\0';
        }
    }
    else
    {
        snprintf(session->prompt, sizeof(session->prompt), "<%s>",
                 session->current_view->name ? session->current_view->name : nn_cli_get_hostname());
    }
}

// Send the prompt to the client
void send_prompt(uint32_t client_fd, nn_cli_session_t *session)
{
    send_message(client_fd, session->prompt);
    send_message(client_fd, " ");
}

// Trim whitespace from both ends of a string
static char *trim(char *str)
{
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str))
    {
        str++;
    }

    if (*str == 0)
    {
        return str;
    }

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
    {
        end--;
    }

    // Write new null terminator
    *(end + 1) = '\0';

    return str;
}

// Help command handler
void cmd_help(uint32_t client_fd, const char *args)
{
    (void)args;
    send_message(client_fd, "\r\nAvailable commands - use TAB for completion, ? for help\r\n");
}

// Exit command handler
void cmd_exit(uint32_t client_fd, const char *args)
{
    (void)args;
    send_message(client_fd, "\r\nGoodbye!\r\n");
    close(client_fd);
}

// Show version command handler
void cmd_show_version(uint32_t client_fd, const char *args)
{
    (void)args;
    send_message(client_fd, "\r\nNetNexus Telnet CLI Server\r\n");
    send_message(client_fd, "Version: 1.0.0\r\n");
    send_message(client_fd, "Build Date: " __DATE__ " " __TIME__ "\r\n");
    send_message(client_fd, "\r\n");
}

// Forward declaration
static void print_view_tree(nn_cli_view_node_t *view, uint32_t client_fd, uint32_t indent);

// Show tree command handler
void cmd_show_tree(uint32_t client_fd, const char *args)
{
    (void)args;

    send_message(client_fd, "\r\nCLI View Tree Structure:\r\n");
    send_message(client_fd, "========================\r\n");

    if (g_view_tree.root)
    {
        // Print view hierarchy
        send_message(client_fd, "\r\nView Hierarchy:\r\n");
        print_view_tree(g_view_tree.root, client_fd, 0);
    }

    send_message(client_fd, "\r\n");
}

// Helper to print view tree
static void print_view_tree(nn_cli_view_node_t *view, uint32_t client_fd, uint32_t indent)
{
    if (!view)
    {
        return;
    }

    char buffer[256];
    char indent_str[64] = "";
    for (uint32_t i = 0; i < indent && i < 30; i++)
    {
        strcat(indent_str, "  ");
    }

    snprintf(buffer, sizeof(buffer), "%s- %s (%d commands)\r\n", indent_str, view->name ? view->name : "unknown",
             view->cmd_tree ? view->cmd_tree->num_children : 0);
    send_message(client_fd, buffer);

    for (uint32_t i = 0; i < view->num_children; i++)
    {
        print_view_tree(view->children[i], client_fd, indent + 1);
    }
}

// Configure command handler
void cmd_configure(uint32_t client_fd, const char *args)
{
    (void)args;
    (void)client_fd;
    // View switching handled in process_command
}

// End command handler
void cmd_end(uint32_t client_fd, const char *args)
{
    (void)args;
    (void)client_fd;
    // View switching handled in process_command
}

// Process a command line
void process_command(uint32_t client_fd, const char *cmd_line, nn_cli_session_t *session)
{
    char buffer[MAX_CMD_LEN];
    strncpy(buffer, cmd_line, MAX_CMD_LEN - 1);
    buffer[MAX_CMD_LEN - 1] = '\0';

    char *trimmed = trim(buffer);

    // Empty command
    if (strlen(trimmed) == 0)
    {
        return;
    }

    // Get current view's command tree
    if (!session->current_view || !session->current_view->cmd_tree)
    {
        send_message(client_fd, "\r\nError: No command tree for current view\r\n");
        return;
    }

    // Match command in tree
    char *remaining_args = NULL;
    nn_cli_tree_node_t *node = nn_cli_tree_match_command(session->current_view->cmd_tree, trimmed, &remaining_args);

    if (node && node->callback)
    {
        // Handle view-switching commands
        if (strcmp(trimmed, "configure") == 0)
        {
            nn_cli_view_node_t *config_view = nn_cli_view_find_by_name(g_view_tree.root, "config");
            if (config_view)
            {
                session->current_view = config_view;
                update_prompt(session);
            }
        }
        else if (strcmp(trimmed, "end") == 0)
        {
            session->current_view = g_view_tree.root;
            update_prompt(session);
        }
        else if (strcmp(trimmed, "exit") == 0)
        {
            if (session->current_view->parent)
            {
                session->current_view = session->current_view->parent;
                update_prompt(session);
            }
            else
            {
                // Root view - actually exit
                node->callback(client_fd, remaining_args);
                free(remaining_args);
                return;
            }
        }

        // Execute callback
        if (strcmp(trimmed, "exit") != 0 || !session->current_view->parent)
        {
            node->callback(client_fd, remaining_args);

            // Update prompt after command execution (in case hostname changed)
            update_prompt(session);
        }
    }
    else if (node && node->num_children > 0)
    {
        // Incomplete command
        send_message(client_fd, "\r\nIncomplete command. Available options:\r\n");
        nn_cli_tree_print_help(node, client_fd);
    }
    else
    {
        // Unknown command
        char error_msg[MAX_CMD_LEN + 64];
        snprintf(error_msg, sizeof(error_msg), "\r\nUnknown command: %s\r\n", trimmed);
        send_message(client_fd, error_msg);
    }

    free(remaining_args);
}

// Initialize CLI from base configuration
uint32_t nn_cli_init(void)
{
    // Free existing tree if any
    nn_cli_cleanup();

    // Load base view structure
    if (nn_cli_xml_load_view_tree("../config/commands_base.xml", &g_view_tree) != 0)
    {
        fprintf(stderr, "Failed to load base CLI views\n");
        return -1;
    }

    printf("CLI base views loaded successfully\n");
    return 0;
}

// Register module commands to existing views
uint32_t nn_cli_register_module(const char *xml_file)
{
    if (!xml_file)
    {
        return -1;
    }

    // Load commands from module file
    if (nn_cli_xml_load_commands(xml_file, &g_view_tree) != 0)
    {
        fprintf(stderr, "Failed to load module from %s\n", xml_file);
        return -1;
    }

    printf("Module loaded: %s\n", xml_file);
    return 0;
}

// Cleanup CLI trees
void nn_cli_cleanup(void)
{
    if (g_view_tree.root)
    {
        nn_cli_view_free(g_view_tree.root);
        g_view_tree.root = NULL;
    }
    if (g_view_tree.global_view)
    {
        nn_cli_view_free(g_view_tree.global_view);
        g_view_tree.global_view = NULL;
    }
}

// Handle client connection
void handle_client(uint32_t client_fd)
{
    char line_buffer[MAX_CMD_LEN];
    uint32_t line_pos = 0;

    // Initialize session
    nn_cli_session_t session;
    session.current_view = g_view_tree.root;
    update_prompt(&session);

    // Enable telnet character mode
    unsigned char telnet_opts[] = {
        255, 251, 1,  // IAC WILL ECHO
        255, 251, 3,  // IAC WILL SUPPRESS_GO_AHEAD
        255, 253, 34, // IAC DO LINEMODE (with MODE 0 for character mode)
    };
    write(client_fd, telnet_opts, sizeof(telnet_opts));

    // Send welcome message
    send_message(client_fd, "\r\n");
    send_message(client_fd, "Welcome to NetNexus CLI\r\n");
    send_message(client_fd, "Type 'help' for available commands\r\n");
    send_message(client_fd, "\r\n");

    // Send initial prompt
    send_prompt(client_fd, &session);

    // Main input loop
    char c;
    while (read(client_fd, &c, 1) > 0)
    {
        // Filter out telnet protocol commands (IAC sequences)
        if ((unsigned char)c == 255)
        { // IAC
            // Read and discard the next 2 bytes (command and option)
            char discard[2];
            read(client_fd, discard, 2);
            continue;
        }

        // Handle Enter
        if (c == '\r' || c == '\n')
        {
            send_message(client_fd, "\r\n");

            if (line_pos > 0)
            {
                line_buffer[line_pos] = '\0';

                // Process command
                process_command(client_fd, line_buffer, &session);

                // Reset line buffer
                line_pos = 0;
            }

            send_prompt(client_fd, &session);
        }
        // Handle Backspace
        else if (c == 127 || c == 8)
        {
            if (line_pos > 0)
            {
                line_pos--;
                send_message(client_fd, "\b \b");
            }
        }
        // Handle TAB
        else if (c == '\t')
        {
            line_buffer[line_pos] = '\0';

            if (session.current_view && session.current_view->cmd_tree)
            {
                // Try to match the current input to find context
                char *remaining = NULL;
                nn_cli_tree_node_t *context =
                    nn_cli_tree_match_command(session.current_view->cmd_tree, line_buffer, &remaining);

                // Determine what to complete
                nn_cli_tree_node_t *search_root = NULL;
                char *search_prefix = "";

                // Check if we have a trailing space - indicates we want subcommands
                uint32_t has_trailing_space = (line_pos > 0 && line_buffer[line_pos - 1] == ' ');

                if (context && context != session.current_view->cmd_tree && has_trailing_space)
                {
                    // Matched a command with trailing space - show its children
                    search_root = context;
                    search_prefix = "";
                }
                else if (context && context != session.current_view->cmd_tree && remaining && strlen(remaining) > 0)
                {
                    // Matched a command with remaining text - complete from its children
                    search_root = context;
                    search_prefix = remaining;
                }
                else
                {
                    // No match or at root - complete from root
                    search_root = session.current_view->cmd_tree;
                    search_prefix = line_buffer;

                    // Trim trailing space from prefix
                    if (has_trailing_space && strlen(search_prefix) > 0)
                    {
                        char *temp = strdup(search_prefix);
                        temp[strlen(temp) - 1] = '\0';
                        search_prefix = temp;
                    }
                }

                // Find partial matches
                nn_cli_tree_node_t *matches[50];
                uint32_t num_matches = 0;

                if (search_root)
                {
                    for (uint32_t i = 0; i < search_root->num_children && num_matches < 50; i++)
                    {
                        nn_cli_tree_node_t *child = search_root->children[i];
                        if (child->name)
                        {
                            // Check if child name starts with prefix
                            uint32_t prefix_len = strlen(search_prefix);
                            if (prefix_len == 0 || strncmp(child->name, search_prefix, prefix_len) == 0)
                            {
                                matches[num_matches++] = child;
                            }
                        }
                    }
                }

                if (num_matches == 1 && !has_trailing_space)
                {
                    // Single match without trailing space - complete it
                    const char *match_name = matches[0]->name;
                    uint32_t prefix_len = strlen(search_prefix);

                    // Add the remaining characters
                    for (uint32_t i = prefix_len; match_name[i] && line_pos < MAX_CMD_LEN - 1; i++)
                    {
                        line_buffer[line_pos++] = match_name[i];
                        write(client_fd, &match_name[i], 1);
                    }

                    // Add space after completion
                    if (line_pos < MAX_CMD_LEN - 1)
                    {
                        line_buffer[line_pos++] = ' ';
                        write(client_fd, " ", 1);
                    }
                }
                else if (num_matches == 1 && has_trailing_space)
                {
                    // Single match with trailing space - just show hint
                    send_message(client_fd, "\r\n");
                    char option[256];
                    snprintf(option, sizeof(option), "  %-20s - %s\r\n", matches[0]->name,
                             matches[0]->description ? matches[0]->description : "");
                    send_message(client_fd, option);
                    send_prompt(client_fd, &session);
                    send_message(client_fd, line_buffer);
                }
                else if (num_matches > 1)
                {
                    // Multiple matches - show options
                    send_message(client_fd, "\r\n");
                    for (uint32_t i = 0; i < num_matches; i++)
                    {
                        char option[256];
                        snprintf(option, sizeof(option), "  %-20s - %s\r\n", matches[i]->name,
                                 matches[i]->description ? matches[i]->description : "");
                        send_message(client_fd, option);
                    }
                    send_prompt(client_fd, &session);
                    send_message(client_fd, line_buffer);
                }
                else if (num_matches == 0 && search_root && search_root->num_children > 0)
                {
                    // No matches but context has children - show all children
                    send_message(client_fd, "\r\n");
                    for (uint32_t i = 0; i < search_root->num_children; i++)
                    {
                        char option[256];
                        snprintf(option, sizeof(option), "  %-20s - %s\r\n", search_root->children[i]->name,
                                 search_root->children[i]->description ? search_root->children[i]->description : "");
                        send_message(client_fd, option);
                    }
                    send_prompt(client_fd, &session);
                    send_message(client_fd, line_buffer);
                }

                free(remaining);
            }
        }
        // Handle ?
        else if (c == '?')
        {
            line_buffer[line_pos] = '\0';
            send_message(client_fd, "\r\n");

            if (session.current_view && session.current_view->cmd_tree)
            {
                // Try to match the current input to find context (same as TAB)
                char *remaining = NULL;
                nn_cli_tree_node_t *context =
                    nn_cli_tree_match_command(session.current_view->cmd_tree, line_buffer, &remaining);

                // Determine what to show
                nn_cli_tree_node_t *help_root = NULL;

                // Check if we have a trailing space
                uint32_t has_trailing_space = (line_pos > 0 && line_buffer[line_pos - 1] == ' ');

                if (context && context != session.current_view->cmd_tree && has_trailing_space)
                {
                    // Matched a command with trailing space - show its children
                    help_root = context;
                }
                else if (context && context != session.current_view->cmd_tree && remaining && strlen(remaining) > 0)
                {
                    // Matched a command with remaining text - show its children
                    help_root = context;
                }
                else
                {
                    // No match or at root - show root commands
                    help_root = session.current_view->cmd_tree;
                }

                // Print help for the determined context
                if (help_root)
                {
                    nn_cli_tree_print_help(help_root, client_fd);
                }

                free(remaining);
            }

            send_prompt(client_fd, &session);
            send_message(client_fd, line_buffer);
        }
        // Regular character
        else if (line_pos < MAX_CMD_LEN - 1 && c >= 32 && c < 127)
        {
            line_buffer[line_pos++] = c;
            write(client_fd, &c, 1);
        }
    }
}

// Scan directory and register all command modules
uint32_t nn_cli_register_all_modules(const char *config_dir)
{
    if (!config_dir)
    {
        return -1;
    }

    DIR *dir = opendir(config_dir);
    if (!dir)
    {
        fprintf(stderr, "Failed to open config directory: %s\n", config_dir);
        return -1;
    }

    struct dirent *entry;
    uint32_t loaded_count = 0;
    char filepath[512];

    while ((entry = readdir(dir)) != NULL)
    {
        // Skip if not a regular file
        if (entry->d_type != DT_REG)
        {
            continue;
        }

        // Check if filename starts with "commands_" and ends with ".xml"
        const char *filename = entry->d_name;
        size_t len = strlen(filename);

        if (len > 13 && // "commands_X.xml" minimum length
            strncmp(filename, "commands_", 9) == 0 && strcmp(filename + len - 4, ".xml") == 0)
        {
            // Build full path
            snprintf(filepath, sizeof(filepath), "%s/%s", config_dir, filename);

            // Register module
            if (nn_cli_register_module(filepath) == 0)
            {
                loaded_count++;
            }
        }
    }

    closedir(dir);

    printf("Total modules loaded: %u\n", loaded_count);
    return 0;
}

// Sysname command handler
void cmd_sysname(uint32_t client_fd, const char *args)
{
    if (!args || strlen(args) == 0)
    {
        // Show current hostname
        char msg[128];
        snprintf(msg, sizeof(msg), "\r\nCurrent hostname: %s\r\n", nn_cli_get_hostname());
        send_message(client_fd, msg);
        return;
    }

    // Trim whitespace
    while (isspace((unsigned char)*args))
    {
        args++;
    }

    if (strlen(args) == 0)
    {
        send_message(client_fd, "\r\nError: Hostname cannot be empty\r\n");
        return;
    }

    if (strlen(args) >= MAX_HOSTNAME_LEN)
    {
        send_message(client_fd, "\r\nError: Hostname too long (max 63 characters)\r\n");
        return;
    }

    // Set new hostname
    nn_cli_set_hostname(args);

    char msg[128];
    snprintf(msg, sizeof(msg), "\r\nHostname set to: %s\r\n", nn_cli_get_hostname());
    send_message(client_fd, msg);
}
