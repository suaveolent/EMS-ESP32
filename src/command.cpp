/*
 * EMS-ESP - https://github.com/emsesp/EMS-ESP
 * Copyright 2020-2024  Paul Derbyshire
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "command.h"
#include "emsdevice.h"
#include "emsesp.h"

namespace emsesp {

uuid::log::Logger Command::logger_{F_(command), uuid::log::Facility::DAEMON};

std::vector<Command::CmdFunction> Command::cmdfunctions_;

// takes a path and a json body, parses the data and calls the command
// the path is leading so if duplicate keys are in the input JSON it will be ignored
// the entry point will be either via the Web API (api/) or MQTT (<base>/)
// returns a return code and json output
uint8_t Command::process(const char * path, const bool is_admin, const JsonObject input, JsonObject output) {
    SUrlParser p; // parse URL for the path names
    p.parse(path);

    if (!p.paths().size()) {
        return message(CommandRet::ERROR, "invalid path", output);
    }

    // check first if it's from API, if so strip the "api/"
    if (p.paths().front() == "api") {
        p.paths().erase(p.paths().begin());
    } else {
        // not /api, so must be MQTT path. Check for base and remove it.
        if (!strncmp(path, Mqtt::base().c_str(), Mqtt::base().length())) {
            char new_path[Mqtt::MQTT_TOPIC_MAX_SIZE];
            strlcpy(new_path, path, sizeof(new_path));
            p.parse(new_path + Mqtt::base().length() + 1); // re-parse the stripped path
        } else {
            return message(CommandRet::ERROR, "unrecognized path", output); // error
        }
    }

    // re-calculate new path
    // if there is only a path (URL) and no body then error!
    size_t num_paths = p.paths().size();
    if (!num_paths && !input.size()) {
        return message(CommandRet::ERROR, "missing command in path", output);
    }

    std::string cmd_s;
    int8_t      id_n = -1; // default hc

    // check for a device as first item in the path
    // if its not a known device (thermostat, boiler etc) look for any special MQTT subscriptions
    const char * device_s = nullptr;
    if (!num_paths) {
        // we must look for the device in the JSON body
        if (input.containsKey("device")) {
            device_s = input["device"];
        }
    } else {
        // extract it from the path
        device_s = p.paths().front().c_str(); // get the device (boiler, thermostat, system etc)
    }

    // validate the device, make sure it exists
    uint8_t device_type = EMSdevice::device_name_2_device_type(device_s);
    if (!device_has_commands(device_type)) {
        LOG_DEBUG("Command failed: unknown device '%s'", device_s);
        return message(CommandRet::ERROR, "unknown device", output);
    }

    // the next value on the path should be the command or entity name
    const char * command_p = nullptr;
    if (num_paths == 2) {
        command_p = p.paths()[1].c_str();
    } else if (num_paths == 3) {
        // concatenate the path into one string as it could be in the format 'hc/XXX'
        char command[COMMAND_MAX_LENGTH];
        snprintf(command, sizeof(command), "%s/%s", p.paths()[1].c_str(), p.paths()[2].c_str());
        command_p = command;
    } else if (num_paths > 3) {
        // concatenate the path into one string as it could be in the format 'hc/XXX/attribute'
        char command[COMMAND_MAX_LENGTH];
        snprintf(command, sizeof(command), "%s/%s/%s", p.paths()[1].c_str(), p.paths()[2].c_str(), p.paths()[3].c_str());
        command_p = command;
    } else {
        // take it from the JSON
        if (input.containsKey("entity")) {
            command_p = input["entity"];
        } else if (input.containsKey("cmd")) {
            command_p = input["cmd"];
        }
    }

    // some commands may be prefixed with hc. dhw. or hc/ or dhw/ so extract these if they exist
    // parse_command_string returns the extracted command
    command_p = parse_command_string(command_p, id_n);
    if (command_p == nullptr) {
        // handle dead endpoints like api/system or api/boiler
        // default to 'info' for SYSTEM, the other devices to 'values' for shortname version
        if (num_paths < (id_n > 0 ? 4 : 3)) {
            command_p = device_type == EMSdevice::DeviceType::SYSTEM ? F_(info) : F_(values);
        } else {
            return message(CommandRet::NOT_FOUND, "missing or bad command", output);
        }
    }

    // if we don't have an id/hc/dhw try and get it from the JSON input
    // it's allowed to have no id, and then keep the default to -1
    if (id_n == -1) {
        if (input.containsKey("hc")) {
            id_n = input["hc"];
        } else if (input.containsKey("dhw")) {
            id_n = input["dhw"];
            id_n += DeviceValueTAG::TAG_DHW1 - DeviceValueTAG::TAG_HC1; // dhw1 has id 9
        } else if (input.containsKey("id")) {
            id_n = input["id"];
        } else if (input.containsKey("ahs")) {
            id_n = input["ahs"];
            id_n += DeviceValueTAG::TAG_AHS1 - DeviceValueTAG::TAG_HC1; // ahs1 has id 19
        } else if (input.containsKey("hs")) {
            id_n = input["hs"];
            id_n += DeviceValueTAG::TAG_HS1 - DeviceValueTAG::TAG_HC1; // hs1 has id 20
        }
    }

    // the value must always come from the input JSON. It's allowed to be empty.
    JsonVariant data;
    if (input.containsKey("data")) {
        data = input["data"];
    } else if (input.containsKey("value")) {
        data = input["value"];
    }

    // check if data is entity like device/hc/name/value
    if (data.is<const char *>()) {
        const char * d = data.as<const char *>();
        if (strlen(d)) {
            char * device_end = (char *)strchr(d, '/');
            if (device_end != nullptr) {
                char         device_s[20] = {'\0'};
                const char * device_p     = device_s;
                const char * data_p       = nullptr;
                strlcpy(device_s, d, device_end - d + 1);
                data_p      = device_end + 1;
                int8_t id_d = -1;
                data_p      = parse_command_string(data_p, id_d);
                if (data_p == nullptr) {
                    return CommandRet::INVALID;
                }
                char data_s[COMMAND_MAX_LENGTH];
                strlcpy(data_s, Helpers::toLower(data_p).c_str(), 30);
                if (strstr(data_s, "/value") == nullptr) {
                    strcat(data_s, "/value");
                }
                uint8_t device_type = EMSdevice::device_name_2_device_type(device_p);
                if (CommandRet::OK != Command::call(device_type, data_s, "", true, id_d, output)) {
                    return CommandRet::INVALID;
                }
                if (!output.containsKey("api_data")) {
                    return CommandRet::INVALID;
                }
                String dat = output["api_data"].as<String>();
                output.clear();
                return Command::call(device_type, command_p, dat.c_str(), is_admin, id_n, output);
            }
        }
    }

    // call the command based on the type
    uint8_t return_code = CommandRet::ERROR;
    if (data.is<const char *>()) {
        return_code = Command::call(device_type, command_p, data.as<const char *>(), is_admin, id_n, output);
    } else if (data.is<int>()) {
        char data_str[10];
        return_code = Command::call(device_type, command_p, Helpers::itoa(data.as<int32_t>(), data_str), is_admin, id_n, output);
    } else if (data.is<float>()) {
        char data_str[10];
        return_code = Command::call(device_type, command_p, Helpers::render_value(data_str, data.as<float>(), 2), is_admin, id_n, output);
    } else if (data.is<bool>()) {
        return_code = Command::call(device_type, command_p, data.as<bool>() ? "1" : "0", is_admin, id_n, output);
    } else if (data.isNull()) {
        return_code = Command::call(device_type, command_p, "", is_admin, id_n, output); // empty, will do a query instead
    } else {
        return message(CommandRet::ERROR, "cannot parse command", output); // can't process
    }
    return return_code;
}

std::string Command::return_code_string(const uint8_t return_code) {
    switch (return_code) {
    case CommandRet::ERROR:
        return "Error";
    case CommandRet::OK:
        return "OK";
    case CommandRet::NOT_FOUND:
        return "Not Found";
    case CommandRet::NOT_ALLOWED:
        return "Not Authorized";
    case CommandRet::FAIL:
        return "Failed";
    case CommandRet::INVALID:
        return "Invalid";
    default:
        break;
    }
    char s[4];
    return Helpers::smallitoa(s, return_code);
}

// takes a string like "hc1/seltemp" or "seltemp" or "dhw2.seltemp" and tries to get the id and cmd
// returns start position of the command string
const char * Command::parse_command_string(const char * command, int8_t & id) {
    if (command == nullptr) {
        return nullptr;
    }

    // convert cmd to lowercase and compare
    char * lowerCmd = strdup(command);
    for (char * p = lowerCmd; *p; p++) {
        *p = tolower(*p);
    }

    // check prefix and valid number range, also check 'id'
    if (!strncmp(lowerCmd, "hc", 2) && command[2] >= '1' && command[2] <= '8') {
        id = command[2] - '0';
        command += 3;
    } else if (!strncmp(lowerCmd, "dhw", 3) && command[3] == '1' && command[4] == '0') {
        id = DeviceValueTAG::TAG_DHW10; //18;
        command += 5;
    } else if (!strncmp(lowerCmd, "dhw", 3) && command[3] >= '1' && command[3] <= '9') {
        id = command[3] - '1' + DeviceValueTAG::TAG_DHW1; //9;
        command += 4;
    } else if (!strncmp(lowerCmd, "id", 2) && command[2] == '1' && command[3] >= '0' && command[3] <= '9') {
        id = command[3] - '0' + 10;
        command += 4;
    } else if (!strncmp(lowerCmd, "id", 2) && command[2] >= '1' && command[2] <= '9') {
        id = command[2] - '0';
        command += 3;
    } else if (!strncmp(lowerCmd, "ahs", 3) && command[3] >= '1' && command[3] <= '1') { // only ahs1 for now
        id = command[3] - '1' + DeviceValueTAG::TAG_AHS1;                                // 19;
        command += 4;
    } else if (!strncmp(lowerCmd, "hs", 2) && command[2] == '1' && command[3] >= '0' && command[3] <= '6') {
        id = command[3] - '0' + DeviceValueTAG::TAG_HS10; //29;
        command += 4;
    } else if (!strncmp(lowerCmd, "hs", 2) && command[2] >= '1' && command[2] <= '9') {
        id = command[2] - '1' + DeviceValueTAG::TAG_HS1; //20;
        command += 3;
    } else if (!strncmp(lowerCmd, "dhw", 3)) { // no number
        id = DeviceValueTAG::TAG_DHW1;
        command += 3;
    }

    // remove separator
    if (command[0] == '/' || command[0] == '.' || command[0] == '_') {
        command++;
    }

    free(lowerCmd);

    // return null for empty command
    if (command[0] == '\0') {
        return nullptr;
    }

    return command;
}

// calls a command directly
uint8_t Command::call(const uint8_t device_type, const char * cmd, const char * value, const int8_t id) {
    // create a temporary buffer
    JsonDocument output_doc;
    JsonObject   output = output_doc.to<JsonObject>();

    // authenticated is always true and ID is the default value
    return call(device_type, cmd, value, true, id, output);
}

// calls a command. Takes a json object for output.
// id may be used to represent a heating circuit for example
// returns 0 if the command errored, 1 (TRUE) if ok, 2 if not found, 3 if error or 4 if not allowed
uint8_t Command::call(const uint8_t device_type, const char * cmd, const char * value, const bool is_admin, const int8_t id, JsonObject output) {
    if (cmd == nullptr) {
        return CommandRet::NOT_FOUND;
    }

    auto dname = EMSdevice::device_type_2_device_name(device_type); // device name, not translated

    // check first if there is a command given as it may be calling a device's attribute (e.g. /api/boiler/nrgheat)
    bool single_command = (!value || !strlen(value));
    if (single_command) {
        if (EMSESP::get_device_value_info(output, cmd, id, device_type)) { // entity = cmd
            LOG_DEBUG("Fetched device entity attributes for %s/%s", dname, cmd);
            return CommandRet::OK;
        } else {
            // char error[100];
            // snprintf(error, sizeof(error), "no data for device %s", dname);
            // output["message"] = error;
        }
    }

    uint8_t device_id = EMSESP::device_id_from_cmd(device_type, cmd, id);

    // determine flags based on id (which is the tag)
    uint8_t flag = CommandFlag::CMD_FLAG_DEFAULT;
    int8_t  tag  = id;
    if (tag >= DeviceValueTAG::TAG_HC1 && tag <= DeviceValueTAG::TAG_HC8) {
        flag = CommandFlag::CMD_FLAG_HC;
    } else if (tag >= DeviceValueTAG::TAG_DHW1 && tag <= DeviceValueTAG::TAG_DHW10) {
        flag = CommandFlag::CMD_FLAG_DHW;
    } else if (tag >= DeviceValueTAG::TAG_HS1 && tag <= DeviceValueTAG::TAG_HS16) {
        flag = CommandFlag::CMD_FLAG_HS;
    } else if (tag >= DeviceValueTAG::TAG_AHS1 && tag <= DeviceValueTAG::TAG_AHS1) {
        flag = CommandFlag::CMD_FLAG_AHS;
    }

    // first see if there is a command registered and it's valid
    auto cf = find_command(device_type, device_id, cmd, flag);
    if (!cf) {
        LOG_WARNING("Command failed: invalid command '%s'", cmd ? cmd : "");
        return CommandRet::ERROR;
    }

    // before calling the command, check permissions and abort if not authorized
    if (cf->has_flags(CommandFlag::ADMIN_ONLY) && !is_admin) {
        LOG_WARNING("Command failed: authentication failed");
        output["message"] = "authentication failed";
        return CommandRet::NOT_ALLOWED; // command not allowed
    }

    // build up the log string for reporting back
    // We send the log message as Warning so it appears in the log (debug is only enabled when compiling with DEBUG)
    std::string ro          = EMSESP::system_.readonly_mode() ? "[readonly] " : "";
    auto        description = Helpers::translated_word(cf->description_);
    char        info_s[100];
    if (strlen(description)) {
        snprintf(info_s, sizeof(info_s), "'%s/%s' (%s)", dname, cmd, description);
    } else {
        snprintf(info_s, sizeof(info_s), "'%s/%s'", dname, cmd);
    }
    if (single_command) {
        LOG_DEBUG(("%sCalling command %s"), ro.c_str(), info_s);
    } else {
        if (id > 0) {
            LOG_INFO(("%sCalling command %s with value %s and id %d on device 0x%02X"), ro.c_str(), info_s, value, id, device_id);
        } else {
            LOG_INFO(("%sCalling command %s with value %s"), ro.c_str(), info_s, value);
        }
    }

    // call the function based on type, either with a json package or no parameters
    uint8_t return_code = CommandRet::OK;
    if (cf->cmdfunction_json_) {
        // JSON
        return_code = ((cf->cmdfunction_json_)(value, id, output)) ? CommandRet::OK : CommandRet::ERROR;
    } else if (cf->cmdfunction_) {
        // Normal command
        if (!single_command && EMSESP::cmd_is_readonly(device_type, device_id, cmd, id)) {
            return_code = CommandRet::INVALID; // error on readonly or invalid hc
        } else {
            return_code = ((cf->cmdfunction_)(value, id)) ? CommandRet::OK : CommandRet::ERROR;
        }
    }

    // report back. If not OK show output from error, other return the HTTP code
    if (return_code != CommandRet::OK) {
        if (single_command) {
            LOG_ERROR("Command '%s' failed with error '%s'", cmd, FL_(cmdRet)[return_code]);
        } else {
            LOG_ERROR("Command '%s: %s' failed with error '%s'", cmd, value, FL_(cmdRet)[return_code]);
        }
        return message(return_code, "callback function failed", output);
    }
    return return_code;
}

// add a command to the list, which does not return json
void Command::add(const uint8_t device_type, const uint8_t device_id, const char * cmd, const cmd_function_p cb, const char * const * description, uint8_t flags) {
    // if the command already exists for that device type don't add it
    if (find_command(device_type, device_id, cmd, flags) != nullptr) {
        return;
    }

    // if the description is empty, it's hidden which means it will not show up in Web API or Console as an available command
    if (!description) {
        flags |= CommandFlag::HIDDEN;
    }

    cmdfunctions_.emplace_back(device_type, device_id, flags, cmd, cb, nullptr, description); // callback for json is nullptr
}

// add a command with no json output
// system/temperature/analog devices uses device_id 0
void Command::add(const uint8_t device_type, const char * cmd, const cmd_function_p cb, const char * const * description, uint8_t flags) {
    add(device_type, 0, cmd, cb, description, flags);
}

// add a command to the list, which does return a json object as output
void Command::add(const uint8_t device_type, const char * cmd, const cmd_json_function_p cb, const char * const * description, uint8_t flags) {
    // if the command already exists for that device type don't add it
    if (find_command(device_type, 0, cmd, flags) != nullptr) {
        return;
    }

    cmdfunctions_.emplace_back(device_type, 0, flags, cmd, nullptr, cb, description); // callback for json is included
}

// see if a command exists for that device type
// is not case sensitive
Command::CmdFunction * Command::find_command(const uint8_t device_type, const uint8_t device_id, const char * cmd, const uint8_t flag) {
    if ((cmd == nullptr) || (strlen(cmd) == 0) || (cmdfunctions_.empty())) {
        return nullptr;
    }

    for (auto & cf : cmdfunctions_) {
        if (Helpers::toLower(cmd) == Helpers::toLower(cf.cmd_) && (cf.device_type_ == device_type) && (!device_id || cf.device_id_ == device_id)
            && (flag == CommandFlag::CMD_FLAG_DEFAULT || (flag & 0x3F) == (cf.flags_ & 0x3F))) {
            return &cf;
        }
    }

    return nullptr; // command not found
}

void Command::erase_device_commands(const uint8_t device_type) {
    if (cmdfunctions_.empty()) {
        return;
    }
    auto it = cmdfunctions_.end();
    do {
        int i = it - cmdfunctions_.begin();
        if (cmdfunctions_[i].device_type_ == device_type) {
            cmdfunctions_.erase(it);
        }
    } while (it-- > cmdfunctions_.begin());
}

void Command::erase_command(const uint8_t device_type, const char * cmd, uint8_t flag) {
    if ((cmd == nullptr) || (strlen(cmd) == 0) || (cmdfunctions_.empty())) {
        return;
    }
    auto it = cmdfunctions_.begin();
    for (auto & cf : cmdfunctions_) {
        if (Helpers::toLower(cmd) == Helpers::toLower(cf.cmd_) && (cf.device_type_ == device_type) && ((flag & 0x3F) == (cf.flags_ & 0x3F))) {
            cmdfunctions_.erase(it);
            return;
        }
        it++;
    }
}

// get the tagged command
std::string Command::tagged_cmd(const std::string & cmd, const uint8_t flag) {
    switch (flag & 0x3F) {
    case CommandFlag::CMD_FLAG_HC:
        return "[hc<n>.]" + cmd;
    case CommandFlag::CMD_FLAG_DHW:
        return "dhw[n]." + cmd;
    case CommandFlag::CMD_FLAG_HS:
        return "hs<n>." + cmd;
    case CommandFlag::CMD_FLAG_AHS:
        return "ahs<n>." + cmd;
    default:
        return cmd;
    }
}

// list all commands for a specific device, output as json
bool Command::list(const uint8_t device_type, JsonObject output) {
    // force add info and commands for those non-EMS devices
    if (device_type == EMSdevice::DeviceType::TEMPERATURESENSOR || device_type == EMSdevice::DeviceType::ANALOGSENSOR) {
        output[F_(info)]     = Helpers::translated_word(FL_(info_cmd));
        output[F_(commands)] = Helpers::translated_word(FL_(commands_cmd));
    } else if (cmdfunctions_.empty()) {
        output["message"] = "no commands available";
        return false;
    }

    // create a list of commands we have registered, and sort them
    std::list<std::string> sorted_cmds;
    for (const auto & cf : cmdfunctions_) {
        if ((cf.device_type_ == device_type) && !cf.has_flags(CommandFlag::HIDDEN)) {
            sorted_cmds.push_back(tagged_cmd(cf.cmd_, cf.flags_));
        }
    }
    sorted_cmds.sort();

    for (const auto & cl : sorted_cmds) {
        for (const auto & cf : cmdfunctions_) {
            if ((cf.device_type_ == device_type) && !cf.has_flags(CommandFlag::HIDDEN) && cf.description_ && (cl == tagged_cmd(cf.cmd_, cf.flags_))) {
                output[cl] = Helpers::translated_word(cf.description_);
            }
        }
    }
    return true;
}

// output list of all commands to console for a specific DeviceType
void Command::show(uuid::console::Shell & shell, uint8_t device_type, bool verbose) {
    // create list of commands we have registered
    std::list<std::string> sorted_cmds;
    for (const auto & cf : cmdfunctions_) {
        if ((cf.device_type_ == device_type) && !cf.has_flags(CommandFlag::HIDDEN)) {
            sorted_cmds.push_back(tagged_cmd(cf.cmd_, cf.flags_));
        }
    }

    // non EMS devices always have an info and commands command
    bool show_info = (device_type == EMSdevice::DeviceType::TEMPERATURESENSOR || device_type == EMSdevice::DeviceType::ANALOGSENSOR
                      || device_type == EMSdevice::DeviceType::SCHEDULER || device_type == EMSdevice::DeviceType::CUSTOM);

    if (!verbose && show_info) {
        sorted_cmds.push_back(F_(info));
        sorted_cmds.push_back(F_(commands));
    }

    sorted_cmds.sort(); // sort them

    // if not in verbose mode, just print them on a single line and exit
    if (!verbose) {
        for (const auto & cl : sorted_cmds) {
            shell.print(cl);
            shell.print(" ");
        }
        shell.println();
        return;
    }

    // verbose mode
    shell.printfln("\n%s%s %s:%s", COLOR_BOLD_ON, COLOR_YELLOW, EMSdevice::device_type_2_device_name(device_type), COLOR_RESET);

    // we hard code 'info' and 'commmands' commands so print them first
    if (show_info) {
        shell.printf("  info:\t\t\t\t%slists all values %s*", COLOR_BRIGHT_CYAN, COLOR_BRIGHT_GREEN);
        shell.println(COLOR_RESET);
        shell.printf("  commands:\t\t\t%slists all commands %s*", COLOR_BRIGHT_CYAN, COLOR_BRIGHT_GREEN);
        shell.println(COLOR_RESET);
    }

    for (const auto & cl : sorted_cmds) {
        // find and print the description
        for (const auto & cf : cmdfunctions_) {
            if ((cf.device_type_ == device_type) && !cf.has_flags(CommandFlag::HIDDEN) && cf.description_ && (cl == tagged_cmd(cf.cmd_, cf.flags_))) {
                uint8_t i = cl.length();
                shell.print("  ");
                shell.print(cl);
                // pad with spaces
                while (i++ < 30) {
                    shell.print(' ');
                }
                shell.print(COLOR_BRIGHT_CYAN);
                shell.print(Helpers::translated_word(cf.description_));
                if (!cf.has_flags(CommandFlag::ADMIN_ONLY)) {
                    shell.print(' ');
                    shell.print(COLOR_BRIGHT_GREEN);
                    shell.print('*');
                }
                shell.print(COLOR_RESET);
            }
        }
        shell.println();
    }
}

// see if a device_type is active and has associated commands
// returns false if the device has no commands
bool Command::device_has_commands(const uint8_t device_type) {
    if (device_type == EMSdevice::DeviceType::UNKNOWN) {
        return false;
    }

    if (device_type == EMSdevice::DeviceType::SYSTEM) {
        return true; // we always have System
    }

    // if there are no entries to scheduler/custom/temperaturesensor/analogsensor, don't error but return a message
    if (device_type == EMSdevice::DeviceType::SCHEDULER) {
        return true;
    }

    if (device_type == EMSdevice::DeviceType::CUSTOM) {
        return true;
    }

    if (device_type == EMSdevice::DeviceType::TEMPERATURESENSOR) {
        return EMSESP::sensor_enabled();
    }

    if (device_type == EMSdevice::DeviceType::ANALOGSENSOR) {
        return EMSESP::analog_enabled();
    }

    for (const auto & emsdevice : EMSESP::emsdevices) {
        if (emsdevice && (emsdevice->device_type() == device_type)) {
            // device found, now see if it has any commands
            for (const auto & cf : cmdfunctions_) {
                if (cf.device_type_ == device_type) {
                    return true;
                }
            }
        }
    }

    return false;
}

// list sensors and EMS devices
void Command::show_devices(uuid::console::Shell & shell) {
    shell.printf("%s ", EMSdevice::device_type_2_device_name(EMSdevice::DeviceType::SYSTEM));
    shell.printf("%s ", EMSdevice::device_type_2_device_name(EMSdevice::DeviceType::CUSTOM));
    shell.printf("%s ", EMSdevice::device_type_2_device_name(EMSdevice::DeviceType::SCHEDULER));
    if (EMSESP::sensor_enabled()) {
        shell.printf("%s ", EMSdevice::device_type_2_device_name(EMSdevice::DeviceType::TEMPERATURESENSOR));
    }
    if (EMSESP::analog_enabled()) {
        shell.printf("%s ", EMSdevice::device_type_2_device_name(EMSdevice::DeviceType::ANALOGSENSOR));
    }

    for (const auto & device_class : EMSFactory::device_handlers()) {
        for (const auto & emsdevice : EMSESP::emsdevices) {
            if (emsdevice && (emsdevice->device_type() == device_class.first) && (device_has_commands(device_class.first))) {
                shell.printf("%s ", EMSdevice::device_type_2_device_name(device_class.first));
                break; // we only want to show one (not multiple of the same device types)
            }
        }
    }
    shell.println();
}

// 'show commmands' : output list of all commands to console
// calls show with verbose mode set
void Command::show_all(uuid::console::Shell & shell) {
    shell.printfln("Showing all available commands (%s*%s=authentication not required):", COLOR_BRIGHT_GREEN, COLOR_RESET);

    // show system ones first
    show(shell, EMSdevice::DeviceType::SYSTEM, true);
    show(shell, EMSdevice::DeviceType::CUSTOM, true);
    show(shell, EMSdevice::DeviceType::SCHEDULER, true);

    // then sensors
    if (EMSESP::sensor_enabled()) {
        show(shell, EMSdevice::DeviceType::TEMPERATURESENSOR, true);
    }
    if (EMSESP::analog_enabled()) {
        show(shell, EMSdevice::DeviceType::ANALOGSENSOR, true);
    }

    // now EMS devices, do this in the order of factory classes to keep a consistent order when displaying
    for (const auto & device_class : EMSFactory::device_handlers()) {
        if (Command::device_has_commands(device_class.first)) {
            show(shell, device_class.first, true);
        }
    }

    shell.println();
}

// Extract only the path component from the passed URI and normalized it
// e.g. //one/two////three/// becomes /one/two/three
std::string SUrlParser::path() {
    std::string s = "/"; // set up the beginning slash
    for (const std::string & f : m_folders) {
        s += f;
        s += "/";
    }
    s.pop_back(); // deleting last letter, that is slash '/'
    return std::string(s);
}

SUrlParser::SUrlParser(const char * uri) {
    parse(uri);
}

bool SUrlParser::parse(const char * uri) {
    if (uri == nullptr) {
        return false;
    }

    if (*uri == '\0') {
        return false;
    }

    m_folders.clear();
    m_keysvalues.clear();
    enum Type { begin, folder, param, value };
    std::string s;

    const char * c = uri;
    enum Type    t = Type::begin;
    std::string  last_param;

    do {
        if (*c == '/') {
            if (s.length() > 0) {
                m_folders.push_back(s);
                s.clear();
            }
            t = Type::folder;
        } else if (*c == '?' && (t == Type::folder || t == Type::begin)) {
            if (s.length() > 0) {
                m_folders.push_back(s);
                s.clear();
            }
            t = Type::param;
        } else if (*c == '=' && (t == Type::param || t == Type::begin)) {
            m_keysvalues[s] = "";
            last_param      = s;
            s.clear();
            t = Type::value;
        } else if (*c == '&' && (t == Type::value || t == Type::param || t == Type::begin)) {
            if (t == Type::value) {
                m_keysvalues[last_param] = s;
            } else if ((t == Type::param || t == Type::begin) && (s.length() > 0)) {
                m_keysvalues[s] = "";
                last_param      = s;
            }
            t = Type::param;
            s.clear();
        } else if (*c == '\0' && s.length() > 0) {
            if (t == Type::value) {
                m_keysvalues[last_param] = s;
            } else if (t == Type::folder || t == Type::begin) {
                m_folders.push_back(s);
            } else if (t == Type::param) {
                m_keysvalues[s] = "";
                last_param      = s;
            }
            s.clear();
        } else if (*c == '\0' && s.length() == 0) {
            if (t == Type::param && last_param.length() > 0) {
                m_keysvalues[last_param] = "";
            }
            s.clear();
        } else {
            s += *c;
        }
    } while (*c++ != '\0');

    return true;
}

} // namespace emsesp
