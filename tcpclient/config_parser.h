// ============================================================================
// config_parser.h — JSON configuration file parser
// ============================================================================
// Wraps nlohmann/json (vendored single-header json.hpp) to load
// MsgClientConfig from a JSON file.  See doc/05_Build_and_Run.md
// for the JSON schema and examples.
//
// Returns true on success; on failure error_message contains a
// human-readable description of what went wrong.
// ============================================================================

#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "msg_client.h"
#include <string>

// Parse configuration from JSON file
// Returns true on success, false on failure with error_message set
bool parseConfigFile(const std::string& filepath, 
                     MsgClientConfig& config, 
                     std::string& error_message);

// Print configuration file format documentation
void printConfigFormat();

#endif // CONFIG_PARSER_H
