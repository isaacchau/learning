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
