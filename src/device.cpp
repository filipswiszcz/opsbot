#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "device.h"

constexpr const char *DEVICE_STATE_FILE = "device.dat";

device_status device::initialize(void) noexcept {
    const char *token = std::getenv("TOKEN");
    if (token == nullptr || std::strlen(token) == 0) {
        std::cout << "HOR_ERR: Missing token." << std::endl;
        return device_status::INIT_ERROR;
    }

    std::strncpy(this->token, token, sizeof(this->token) - 1);
    this->token[sizeof(this->token) - 1] = '\0';

    this->is_running = 1;
    this->is_gw_online = 0;
    this->is_mess_online = 0;

    dispatcher_status disp_init_status = this->dispatcher.initialize();
    if (disp_init_status != dispatcher_status::SUCCESS) {
        std::cout << "HOR_ERR: Dispatcher not initialized." << std::endl;
        return device_status::INIT_ERROR;
    }

    gateway_status gate_init_status = this->gateway.initialize(token);
    if (gate_init_status == gateway_status::SUCCESS) {
        this->is_gw_online = 1;
        std::cout << "HOR: Gateway online." << std::endl;
    } else {
        std::cout << "HOR_WARN: Gateway offline." << std::endl;
    }

    this->last_gateway_ping = std::chrono::steady_clock::now(); 

    messenger_status mess_init_status = this->messenger.initialize(&this->dispatcher, &this->requester, token);
    if (mess_init_status == messenger_status::SUCCESS) {
        this->is_mess_online = 1;
        std::cout << "HOR: Messenger online." << std::endl;
    } else {
        std::cout << "HOR_WARN: Messenger offline." << std::endl;
    }

    device_status dev_load_status = this->_load_state();
    if (dev_load_status != device_status::SUCCESS) {
        this->last_p4_commit_id = 0;
        std::cout << "HOR_WARN: Could not access or create state file. Defaulting commit ID to 0." << std::endl;
    }

    device_status dev_chan_status = this->_load_channels();
    if (dev_chan_status != device_status::SUCCESS) {
        std::cout << "HOT_WARN: Channels sync failed." << std::endl;
    }

    this->last_channels_sync = std::chrono::steady_clock::now();

    device_status dev_p4_login = this->_create_p4_ticket();
    if (dev_p4_login != device_status::SUCCESS) {
        std::cout << "HOT_WARN: P4 auth failed." << std::endl;
    }

    this->last_p4_login = std::chrono::steady_clock::now();
    this->last_p4_commits_check = std::chrono::steady_clock::now();

    // this->console.has_pending_cmd.store(0);
    // this->console.worker = std::thread(&device::_console_tick, this);

    return device_status::SUCCESS;
}

void device::tick(void) noexcept {
    while (this->is_running) {
        auto clock_now = std::chrono::steady_clock::now();

        auto gw_ping_elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock_now - this->last_gateway_ping).count();
        if (this->is_gw_online) {
            if (gw_ping_elapsed >= 1) {
                gateway_status gw_ping_status = this->gateway.ping();
                if (gw_ping_status != gateway_status::SUCCESS) {
                    this->is_gw_online = 0;
                    std::cout << "HOR_WARN: Gateway connection dropped." << std::endl;
                }
                this->last_gateway_ping = clock_now;
            }
        } else {
            if (gw_ping_elapsed >= 10) {
                this->_restore_gateway_conn();
                this->last_gateway_ping = clock_now;
            }
        }

        auto dc_chan_sync_elapsed = std::chrono::duration_cast<std::chrono::hours>(clock_now - this->last_channels_sync).count();
        if (dc_chan_sync_elapsed >= 6) {
            device_status dc_chan_sync_status = this->_load_channels();
            if (dc_chan_sync_status != device_status::SUCCESS) {
                std::cout << "HOT_WARN: Channels sync failed." << std::endl;
            }
            this->last_channels_sync = clock_now;
        }

        auto p4_login_elapsed = std::chrono::duration_cast<std::chrono::hours>(clock_now - this->last_p4_login).count();
        if (p4_login_elapsed >= 11) {
            device_status dev_p4_login_status = this->_create_p4_ticket();
            if (dev_p4_login_status != device_status::SUCCESS) {
                std::cout << "HOR_WARN: P4 auth failed." << std::endl;
            }
            this->last_p4_login = clock_now;
        }

        auto p4_commit_check_elapsed = std::chrono::duration_cast<std::chrono::seconds>(clock_now - this->last_p4_commits_check).count();
        if (p4_commit_check_elapsed >= 120) {
            for (u32 i = 0; i < this->depots.counter; i++) {
                if (this->depots.is_active[i]) {
                    device_status dev_p4_check_status = this->_check_p4_commits(i);
                    if (dev_p4_check_status != device_status::SUCCESS) {
                        this->depots.is_active[i] = 0;
                        std::cout << "HOR_WARN: Depot " << this->depots.paths[i] << " is unreachable or invalid. Disabling.." << std::endl;
                    }
                }
            }
            this->last_p4_commits_check = clock_now;
        }

        // handle responses from the server (eg /status)
        // this->_process_console_cmd();

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void device::terminate(void) noexcept {
    this->is_running = 0;
    if (this->console.worker.joinable()) {
        this->console.worker.detach();
    }
    this->gateway.terminate();
    this->dispatcher.terminate();
}

const device::cmd_handler device::cmd_dispatch_table[static_cast<u8>(command_opcode::MAX_OPCODES)] {
    nullptr,
    &device::_exec_print_status,
    &device::_exec_print_channels
};

// void _console_print(const char *message) noexcept {
//     if (message == nullptr) return;
//     std::lock_guard<std::mutex> lock(this->console.lock);
//     std::cout << "\r\033[2K" << message << "\ndevice >> " << std::flush;
// };

void device::_exec_print_status(const command &command) noexcept {}
void device::_exec_print_channels(const command &command) noexcept {}

void device::_process_console_cmds(void) noexcept {
    command command;
    while (this->console.commands.pop(command) != queue_status::EMPTY) {
        const u8 opcode = static_cast<u8>(command.opcode);
        if (opcode > 0 && opcode < static_cast<u8>(command_opcode::MAX_OPCODES)) {
            if (this->cmd_dispatch_table[opcode] == nullptr) {
                continue;
            }
            (this->*cmd_dispatch_table[opcode])(command);
        }
    }
}

// void device::_console_tick(void) noexcept {
//     char input[256];
//     while (this->is_running) {
//         std::cout << "device >> " << std::flush;
//         if (std::fgets(input, sizeof(input), stdin) != nullptr) {
//             size_t input_len = std::strlen(input);
//             if (input_len > 0 && (input[input_len - 1] == '\n' || input[input_len - 1] == '\r')) {
//                 input[input_len - 1] = '\0';
//             }
//             std::lock_guard<std::mutex> lock(this->console.lock);
//             std::strncpy(this->console.pending_cmd, input, sizeof(this->console.pending_cmd) - 1);
//             this->console.pending_cmd[sizeof(this->console.pending_cmd) - 1] = '\0';
//             this->console.has_pending_cmd.store(1);
//         }
//     }
// }

// void device::_process_console_cmd(void) noexcept {
//     if (this->console.has_pending_cmd.load() != 1) {
//         return;
//     }

//     char command[256];

//     {
//         std::lock_guard<std::mutex> lock(this->console.lock);
//         std::strncpy(command, this->console.pending_cmd, sizeof(command));
//         this->console.has_pending_cmd.store(0);
//     }

//     std::cout << std::endl;

//     if (std::strcmp(command, "status") == 0) {
//         std::cout << "Gateway: " << (this->is_gw_online ? "ONLINE" : "OFFLINE") << std::endl;
//         std::cout << "Messenger: " << (this->is_mess_online ? "ONLINE" : "OFFLINE") << std::endl;
//         std::cout << "Tracking: " << this->depots.counter << " depots" << std::endl;
//     } else if (std::strcmp(command, "channels") == 0) {
//         std::cout << "MAPPED DISCORD CHANNELS:" << std::endl;
//         for (u32 i = 0; i < this->channels.counter; i++) {
//             if (this->channels.parent_ids[i] == 0) {
//                 std::cout << "[category] " << this->channels.names[i] << " (id: " << this->channels.ids[i] << ")" << std::endl;
//                 for (u32 j = 0; j < this->channels.counter; j++) {
//                     if (this->channels.ids[i] == this->channels.parent_ids[j]) {
//                         std::cout << " * " << this->channels.names[j] << " (id: " << this->channels.ids[j] << ")" << std::endl;
//                     }
//                 }
//             }
//         }
//     }
// }

device_status device::_load_state(void) noexcept {
    this->depots.counter = 0;
    FILE *file = std::fopen(DEVICE_STATE_FILE, "r");
    if (file == nullptr) {
        if (errno != ENOENT) {
            return device_status::READ_ERROR;
        }
        // this->last_p4_commit_id = 0;
        return this->_save_state();
    }

    char line[256];
    while (fgets(line, sizeof(line), file) != nullptr) {
        if (this->depots.counter >= PERFORCE_MAX_DEPOTS) {
            break;
        }

        char path[128] = {0};
        char channel[101] = {0};
        i32 commit_id = 0;

        if (std::sscanf(line, "depot=%127[^,],%100[^,],%d", path, channel, &commit_id) == 3) {
            std::strncpy(this->depots.paths[this->depots.counter], path, 127);
            std::strncpy(this->depots.channels[this->depots.counter], channel, 101);
            this->depots.commit_ids[this->depots.counter] = commit_id;
            this->depots.is_active[this->depots.counter] = 1;
            this->depots.counter++;
        }
    }

    (void) std::fclose(file);
    return device_status::SUCCESS;
}

device_status device::_save_state(void) noexcept {
    FILE *file = std::fopen(DEVICE_STATE_FILE, "w");
    if (file == nullptr) {
        return device_status::WRITE_ERROR;
    }

    for (u32 i = 0; i < this->depots.counter; i++) {
        std::fprintf(file, "depot=%s,%s,%d", 
            this->depots.paths[i], 
            this->depots.channels[i], 
            this->depots.commit_ids[i]
        );
    }

    (void) std::fclose(file);
    return device_status::SUCCESS;
}

device_status device::_load_channels(void) noexcept {
    const char *guild = std::getenv("GUILD");
    if (guild == nullptr || std::strlen(guild) == 0) {
        std::cout << "HOR_ERR: Missing guild ID." << std::endl;
        std::cout << "HOR_ERR: Skipping channel sync." << std::endl;
        return device_status::MISSING_GUILD;
    }

    char url[256], headers[256];
    std::snprintf(url, sizeof(url), "%s/guilds/%s/channels", DISCORD_API_URL, guild);
    std::snprintf(headers, sizeof(headers), "Authorization: Bot %s\r\nContent-Type: application/json\r\n", this->token);

    char response[65536] = {0};
    requester_status req_get_channels_status = this->requester.get(url, headers, response, sizeof(response));
    if (req_get_channels_status != requester_status::SUCCESS) {
        std::cout << "HOR_ERR: HTTP request failed to fetch channels." << std::endl;
        return device_status::NETWORK_ERROR;
    }

    decltype(this->channels) channels_copy;
    channels_copy.counter = 0;

    const char *response_pos = response;
    while ((response_pos = std::strstr(response_pos, "\"id\":\"")) != nullptr) {
        if (channels_copy.counter >= DISCORD_MAX_CHANNELS) {
            break;
        }

        const char *object_end = find_json_object_end(response_pos);
        if (object_end == nullptr) {
            response_pos++;
            continue;
        }

        u64 id = 0, parent_id = 0;
        i32 type = -1;
        char name[101] = {0};

        if (std::sscanf(response_pos, "\"id\":\"%llu\"", &id) != 1) {
            response_pos++;
            break;
        }

        const char *object = std::strstr(response_pos + 1, "\"id\":\"");
        const char *type_pos = std::strstr(response_pos, "\"type\":");
        if (type_pos != nullptr && (object == nullptr || type_pos < object)) {
            std::sscanf(type_pos, "\"type\":%d", &type);
        }

        if (type != 0 && type != 4 && type != 5 && type != 15) {
            response_pos = object_end;
            continue;
        }

        const char *name_pos = std::strstr(response_pos, "\"name\":\"");
        if (name_pos != nullptr && name_pos < object_end) {
            name_pos += 8;
            const char *name_end = std::strchr(name_pos, '"');
            if (name_end != nullptr) {
                size_t name_len = (name_end - name_pos);
                if (name_len >= sizeof(name)) {
                    name_len = (sizeof(name) - 1);
                }
                std::strncpy(name, name_pos, name_len);
                name[name_len] = '\0';
            }
        }

        if (name[0] == '\0') {
            response_pos = object_end;
            continue;
        }

        const char *parent_pos = std::strstr(response_pos, "\"parent_id\":\"");
        if (parent_pos != nullptr && (object == nullptr || parent_pos < object)) {
            std::sscanf(parent_pos, "\"parent_id\":\"%llu\"", &parent_id);
        }

        channels_copy.ids[channels_copy.counter] = id;
        std::strncpy(channels_copy.names[channels_copy.counter], name, sizeof(channels_copy.names[channels_copy.counter]) - 1);
        channels_copy.names[channels_copy.counter][sizeof(channels_copy.names[channels_copy.counter]) - 1] = '\0';
        channels_copy.parent_ids[channels_copy.counter] = parent_id;
        channels_copy.counter++;

        response_pos = object_end;
    }
    
    std::memcpy(&this->channels, &channels_copy, sizeof(this->channels));
    std::cout << "HOR: " << this->channels.counter << " channels found." << std::endl;

    return device_status::SUCCESS;
}

u64 device::_get_channel_id(const char *name) noexcept {
    if (name == nullptr) {
        return 0;
    }

    const char *split_pos = std::strchr(name, '.');
    if (split_pos != nullptr) {
        char category[101] = {0};
        char channel[101] = {0};

        size_t category_len = split_pos - name;
        if (category_len >= sizeof(category)) {
            category_len = sizeof(category) - 1;
        }

        std::strncpy(category, name, category_len);
        std::strncpy(channel, split_pos + 1, sizeof(channel) - 1);

        u64 category_id = 0;
        for (u32 i = 0; i < this->channels.counter; i++) {
            if (this->channels.parent_ids[i] == 0 && std::strcmp(this->channels.names[i], category) == 0) {
                category_id = this->channels.ids[i];
                break;
            }
        }

        if (category_id == 0) {
            return 0;
        }

        for (u32 i = 0; i < this->channels.counter; i++) {
            if (this->channels.parent_ids[i] == category_id && std::strcmp(this->channels.names[i], channel) == 0) {
                return this->channels.ids[i];
            }
        }
    } else {
        for (u32 i = 0; i < this->channels.counter; i++) {
            if (std::strcmp(this->channels.names[i], name) == 0) {
                return this->channels.ids[i];
            }
        }
    }

    return 0;
}

device_status device::_restore_gateway_conn(void) noexcept {
    this->gateway.terminate();

    gateway_status gate_init_status = this->gateway.initialize(this->token);
    if (gate_init_status != gateway_status::SUCCESS) {
        std::cout << "HOR_WARN: Gateway offline." << std::endl;
        return device_status::NETWORK_ERROR;
    }

    this->is_gw_online = 1;
    std::cout << "HOR: Gateway connected." << std::endl;

    return device_status::SUCCESS;
}

device_status device::_restart_messenger(void) noexcept {
    return device_status::SUCCESS;
}

device_status device::_create_p4_ticket(void) noexcept {
    const char *pass = std::getenv("PASS");
    if (pass == nullptr || std::strlen(pass) == 0) {
        std::cout << "HOR_ERR: Missing P4 password." << std::endl;
        return device_status::MISSING_PASS;
    }

    FILE *pipe;
#ifdef _WIN32
    pipe = _popen("p4 login", "w");
#else
    pipe = popen("p4 login", "w");
#endif

    if (pipe == nullptr) {
        return device_status::READ_ERROR;
    }

    std::fprintf(pipe, "%s\n", pass);

    i32 exit_code = 0;
#ifdef _WIN32
    exit_code = _pclose(pipe);
#else
    exit_code = pclose(pipe);
#endif

    if (exit_code != 0) {
        std::cout << "HOR_WARN: P4 login failed." << std::endl;
        return device_status::READ_ERROR;
    }

    std::cout << "HOR: P4 authorized." << std::endl;

    return device_status::SUCCESS;
}

device_status device::_check_p4_commits(const u32 depot_id) noexcept {
    if (depot_id >= this->depots.counter) {
        return device_status::READ_ERROR;
    }

    const char *depot_name = this->depots.paths[depot_id];
    const char *channel_name = this->depots.channels[depot_id];
    const i32 commit_id = this->depots.commit_ids[depot_id];

    char cmd_p4_changes[256]; // find the best size
    std::snprintf(cmd_p4_changes, sizeof(cmd_p4_changes), "p4 changes -m 1 -s submitted %s", depot_name);

    FILE *cmd_p4_changes_pipe;
#ifdef _WIN32
    cmd_p4_changes_pipe = _popen(cmd_p4_changes, "r");
#else
    cmd_p4_changes_pipe = popen(cmd_p4_changes, "r");
#endif    
    if (cmd_p4_changes_pipe == nullptr) {
        return device_status::READ_ERROR;
    }

    char cmd_p4_changes_out[128] = {0};
    if (std::fgets(cmd_p4_changes_out, sizeof(cmd_p4_changes_out), cmd_p4_changes_pipe) == nullptr) {
#ifdef _WIN32
        _pclose(cmd_p4_changes_pipe);
#else
        pclose(cmd_p4_changes_pipe);
#endif
        return device_status::READ_ERROR;
    }

    i32 curr_commit_id = 0;
    if (std::sscanf(cmd_p4_changes_out, "Change %d on", &curr_commit_id) != 1) {
#ifdef _WIN32
        _pclose(cmd_p4_changes_pipe);
#else
        pclose(cmd_p4_changes_pipe);
#endif
        return device_status::READ_ERROR;
    }

    if (curr_commit_id <= commit_id) {
#ifdef _WIN32
        _pclose(cmd_p4_changes_pipe);
#else
        pclose(cmd_p4_changes_pipe);
#endif
        return device_status::READ_ERROR;
    }

#ifdef _WIN32
        _pclose(cmd_p4_changes_pipe);
#else
        pclose(cmd_p4_changes_pipe);
#endif

    char cmd_p4_desc[128];
    std::snprintf(cmd_p4_desc, sizeof(cmd_p4_desc), "p4 -Ztag -F \"%%desc%%\" describe -s %d", curr_commit_id);
    
    FILE *cmd_p4_desc_pipe;
#ifdef _WIN32
        cmd_p4_desc_pipe = _popen(cmd_p4_desc, "r");
#else
        cmd_p4_desc_pipe = popen(cmd_p4_desc, "r");
#endif
    if (cmd_p4_desc_pipe == nullptr) {
        return device_status::READ_ERROR;
    }

    char content[1024] = {0}, line[256];
    u32 position = 0;
    while (std::fgets(line, sizeof(line), cmd_p4_desc_pipe) != nullptr) {
        size_t line_len = std::strlen(line);
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[line_len - 1] = '\0';
            line_len--;
        }

        if (line_len == 0) {
            continue;
        }

        char *line_start = line;
        while (*line_start == ' ' || *line_start == '\t') {
            line_start++;
        }

        if (*line_start != '-') {
            position += std::snprintf(content + position, sizeof(content) - position, "- %s\\n", line_start);
        } else {
            position += std::snprintf(content + position, sizeof(content) - position, "%s\\n", line_start);
        }
    }

#ifdef _WIN32
        _pclose(cmd_p4_desc_pipe);
#else
        pclose(cmd_p4_desc_pipe);
#endif

    char message[1536];
    std::snprintf(message, sizeof(message), "CHANGELOG #%d\\n%s", curr_commit_id, content);

    if (commit_id != 0) {
        u64 channel_id = this->_get_channel_id(channel_name);
        if (channel_id != 0) {
            this->messenger.send(channel_id, message);
        }
    }

    this->depots.commit_ids[depot_id] = curr_commit_id;
    this->_save_state();

    return device_status::SUCCESS;
}

i32 main(void) {
    device device;
    device_status dev_init_status = device.initialize();
    if (dev_init_status != device_status::SUCCESS) {
        return 1;
    }
    device.tick();
    device.terminate();
    return 0;
}