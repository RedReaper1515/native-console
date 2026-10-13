// "Add to Steam": writes non-Steam game shortcuts and library artwork for hub
// games into a Steam account. Each shortcut starts the hub with --launch <id>,
// which runs the game with its console window hidden and records play time.
//
// Steam keeps shortcuts in userdata/<account>/config/shortcuts.vdf and artwork
// in userdata/<account>/config/grid/. It reads the file at startup and rewrites
// it on exit, so it must not be running while the hub changes it.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace hub::steam {

struct Account {
    std::string id;       // userdata folder name (32-bit account id)
    std::string persona;  // display name from loginusers.vdf, may be empty
    bool most_recent = false;
    long long timestamp = 0;
};

// Steam install folder from the registry (Windows) or standard locations; "" when not found.
std::string detect_root();
std::vector<Account> list_accounts(const std::string& root);
// The preferred account when it exists, otherwise the most recent sign-in, otherwise the first.
std::string pick_account(const std::vector<Account>& accounts, const std::string& preferred);
std::string account_label(const std::vector<Account>& accounts, const std::string& id);

bool steam_running();
// Asks a running Steam client to exit (steam -shutdown); returns once the request is sent.
bool request_shutdown(const std::string& root, std::string& error);

struct ExportGame {
    std::string id;
    std::string title;
    std::string console_name;  // becomes a Steam tag
    std::string cover_path;    // may be empty
};

struct ExportRequest {
    std::string root;
    std::string account;
    std::string hub_exe;           // absolute path of native_console_hub
    std::string data_dir;          // passed back to the hub as --data-dir
    std::string artwork_manifest;  // records which artwork files the hub wrote
    std::vector<ExportGame> games;
};

struct ExportReport {
    int added = 0;
    int updated = 0;
    int artwork_written = 0;
    int artwork_kept = 0;  // files replaced in Steam by the user, left alone
    std::vector<std::string> warnings;
};

// Adds or updates the games' shortcuts and artwork. The previous shortcuts.vdf
// is kept as shortcuts.vdf.native-console.bak. A file the hub cannot parse is
// never overwritten.
bool export_games(const ExportRequest& request, ExportReport& report, std::string& error);

struct RemoveRequest {
    std::string root;
    std::string account;
    std::string hub_exe;
    std::string artwork_manifest;
    std::vector<std::string> ids;  // empty = every shortcut the hub created
};

// Removes the hub's shortcuts and the artwork the hub wrote for them.
bool remove_games(const RemoveRequest& request, int& removed, std::string& error);

// Ids of the hub games that already have a shortcut in this account.
std::set<std::string> games_in_steam(const std::string& root, const std::string& account, const std::string& hub_exe);

}  // namespace hub::steam
