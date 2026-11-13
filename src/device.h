#ifndef HOR_DEVICE_H
#define HOR_DEVICE_H

#include <chrono>

#include "dispatcher.h"
#include "gateway.h"
#include "memory.h"
#include "messenger.h"
#include "requester.h"

enum class device_status : u8 {
    SUCCESS = 0,
    INIT_ERROR,
    READ_ERROR,
    WRITE_ERROR,
    NETWORK_ERROR,
    MISSING_TOKEN,
    MISSING_PASS,
    MISSING_GUILD
};

enum class command_opcode : u8 {
    NONE = 0,
    PRINT_STATUS,
    PRINT_CHANNELS,
    RELOAD_CONFIG,
    SHUTDOWN,
    MAX_OPCODES
};

struct command {
    command_opcode opcode;
    u32 data;
};

// use allocator for all allocations (poetic)
// make device/messenger configurable/restartable (+console)
// change sizes of eg token[128], channel_name[101]

class device {
public:
    [[nodiscard]] device_status initialize(void) noexcept;
    void tick(void) noexcept;
    void terminate(void) noexcept;
private:
    dispatcher dispatcher;
    gateway gateway;
    messenger messenger;
    requester requester;

    struct {
        std::thread worker;
        // char pending_cmd[64];
        // std::atomic<u8> has_pending_cmd;

        lf_spsc_queue<command, 16> commands;
        std::mutex lock;
    } console;

    struct {
        char names[DISCORD_MAX_CHANNELS][101];
        u64 ids[DISCORD_MAX_CHANNELS];
        u64 parent_ids[DISCORD_MAX_CHANNELS];
        u32 counter;
    } channels;

    struct {
        char paths[PERFORCE_MAX_DEPOTS][128];
        char channels[PERFORCE_MAX_DEPOTS][101];
        i32 commit_ids[PERFORCE_MAX_DEPOTS];
        u8 is_active[PERFORCE_MAX_DEPOTS];
        u32 counter;
    } depots;

    char token[128];

    u8 is_running;

    u8 is_gw_online;
    u8 is_mess_online;

    std::chrono::steady_clock::time_point last_gateway_ping;
    std::chrono::steady_clock::time_point last_channels_sync;

    std::chrono::steady_clock::time_point last_p4_login;
    std::chrono::steady_clock::time_point last_p4_commits_check;
    i32 last_p4_commit_id;

    typedef void (device::*cmd_handler) (const command&) noexcept;

    static const cmd_handler cmd_dispatch_table[static_cast<u8>(command_opcode::MAX_OPCODES)];

    // void _console_print(const char *message) noexcept;

    void _exec_print_status(const command &command) noexcept;
    void _exec_print_channels(const command &command) noexcept;

    void _process_console_cmds(void) noexcept;

    // void _console_tick(void) noexcept;
    // void _process_console_cmd(void) noexcept;

    [[nodiscard]] device_status _load_state(void) noexcept;
    [[nodiscard]] device_status _save_state(void) noexcept;

    [[nodiscard]] device_status _restore_gateway_conn(void) noexcept;
    [[nodiscard]] device_status _restart_messenger(void) noexcept;
    
    [[nodiscard]] device_status _load_channels(void) noexcept;
    [[nodiscard]] u64 _get_channel_id(const char *name) noexcept;

    [[nodiscard]] device_status _create_p4_ticket(void) noexcept;
    [[nodiscard]] device_status _check_p4_commits(const u32 depot_id) noexcept;
};

#endif // !HOR_DEVICE_H