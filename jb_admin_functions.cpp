#include "jb_admin_functions.h"
#include <random>
#include <cstdio>
#include <algorithm>

#define MAX_PLAYERS 64

#define CS_TEAM_NONE 0
#define CS_TEAM_SPECTATOR 1
#define CS_TEAM_T 2
#define CS_TEAM_CT 3

jb_admin_functions g_jb_admin_functions;
PLUGIN_EXPOSE(jb_admin_functions, g_jb_admin_functions);

// SYSTEM API`s
IVEngineServer2* engine = nullptr;
CGlobalVars* gpGlobals = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;

// API
IUtilsApi* utils;
IPlayersApi* players_api;
IJailbreakApi* jailbreak_api;
IAdminApi* admin_api;

// VARS


static bool b_CvarLocked = false;
std::map<std::string, std::string> phrases;

IMySQLClient* mysql_client;
IMySQLConnection* connection;


// =========================================
// VARS
// =========================================
bool b_debug = true;

std::string sBanPermission = "@admin/jb_banct";
std::string sKickCTPermission = "@admin/jb_kickct";
std::string sCheckCTBanPermission = "@admin/jb_checkban";
std::string sUnbanCTPermission = "@admin/jb_unbanct";

//==========================================
// HELPERS
//==========================================

void dbgmsg(const char* format, ...) {
    if (!b_debug) return;
    char buf[1024];
    va_list va;
    va_start(va, format);
    V_vsnprintf(buf, sizeof(buf), format, va);
    va_end(va);
    utils->PrintToChatAll("%s debug | %s", g_PLAPI->GetLogTag(), buf);
    META_CONPRINTF("%s debug | %s\n", g_PLAPI->GetLogTag(), buf);
}



// =========================================
// CONFIGS 
// =========================================

void LoadTranslations() {
    phrases.clear();
    KeyValues* g_kvPhrases = new KeyValues("Phrases");
    const char *pszPath = "addons/translations/jailbreak.phrases.txt";

    if (!g_kvPhrases->LoadFromFile(g_pFullFileSystem, pszPath))
    {
        utils->ErrorLog("%s Failed to load %s", g_PLAPI->GetLogTag(), pszPath);
        delete g_kvPhrases;
        return;
    }

    const char* language = utils->GetLanguage();

    for (KeyValues *pKey = g_kvPhrases->GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey()) {
        phrases[std::string(pKey->GetName())] = std::string(pKey->GetString(language));
    }
    delete g_kvPhrases;
}

const char* GetTranslation(const char* key) {
    auto it = phrases.find(key);
    if (it == phrases.end()) return key;
    else return it->second.c_str();
}

bool OnlyDigits(const char* szString) {
    for (size_t i = 0; szString[i] != '\0'; i++) {
        if (!isdigit(szString[i])) return false;
    }
    return true;
}

void LoadDatabase() {
    KeyValues* databases = new KeyValues("Databases");
    if (!databases->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg")) {
        utils->ErrorLog("%s | Failed to load database config.",g_PLAPI->GetLogTag());
        delete databases;
        return;
    }
    KeyValues* database = databases->FindKey("jailbreak");
    if (!database) {
        utils->ErrorLog("%s | Failed to find \"jailbreak\" database in config.",g_PLAPI->GetLogTag());
        delete databases;
        return;
    }
    MySQLConnectionInfo info;
    info.host = database->GetString("host", "");
    info.user = database->GetString("user", "");
    info.pass = database->GetString("pass", "");
    info.database = database->GetString("database", "");
    info.port = database->GetInt("port", 3306);
    connection = mysql_client->CreateMySQLConnection(info);
    connection->Connect([databases](bool connect) {
        if (!connect) {
            META_CONPRINTF("%s Failed to connect to MySQL\n", g_PLAPI->GetLogTag());
            connection = nullptr;
        } 
        delete databases;
    });
}

std::string TimeConverter(int duration) {
    if (duration == 0) {
        return GetTranslation("BanForever");
    }

    int h = duration / 3600;
    int m = (duration % 3600) / 60;
    int s = duration % 60;

    std::string result;

    if (h > 0) {
        result += std::to_string(h) + " " + GetTranslation("Hours") + " ";
    }

    if (m > 0) {
        result += std::to_string(m) + " " + GetTranslation("Minutes") + " ";
    }

    if (s < 10)
        result += "0";

    result += std::to_string(s) + " " + GetTranslation("Seconds");

    return result;
}

void PrintSlotPrefixed(int iSlot, const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChat(iSlot, buf);
}

void PrintAllPrefixed(const char* content) {
    if (!content || content[0] == '\0') return;
    char buf[512];
    g_SMAPI->Format(buf, sizeof(buf), "%s %s", GetTranslation("Prefix"), content);
    utils->PrintToChatAll(buf);
}

// ===================================
// BAN CT COMMAND
// ===================================

void BanPlayer(int iAdmin, std::string arg1, int iDuration, std::string reason) {
    bool bConsole = (iAdmin == -1);
    uint64_t iAdminSID = 0;
    const char* szAdminName = "Console";

    if (!bConsole) {
        auto cAdmin = CCSPlayerController::FromSlot(iAdmin);
        if (!cAdmin) return;
        iAdminSID = cAdmin->m_steamID;
        szAdminName = cAdmin->GetPlayerName();
    }

    bool isDigits = OnlyDigits(arg1.c_str());
    int targetSlot = -1;
    uint64_t targetSid = 0;
    int iTarget = -1; 
    std::string sTargetName = "Unknown";

    if (isDigits) {
        if (arg1.length() <= 2) {
            targetSlot = atoi(arg1.c_str());
        } else if (arg1.length() == 17) { 
            targetSid = std::stoull(arg1);
            sTargetName = arg1; 
        }
    }


    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players_api->IsFakeClient(i)) continue;
        
        auto controller = CCSPlayerController::FromSlot(i);
        if (!controller || controller->m_steamID == 0) continue;

        if (targetSlot != -1 && i == targetSlot) {
            iTarget = i;
            targetSid = controller->m_steamID; 
            sTargetName = controller->GetPlayerName(); 
            break;
        } 
        else if (targetSid != 0 && controller->m_steamID == targetSid) {
            iTarget = i;
            sTargetName = controller->GetPlayerName(); 
            break;
        }
    }


    if (iTarget == -1 && targetSid == 0) {
        if (bConsole) META_CONPRINTF("[Jailbreak] Target not found.\n");
        else PrintSlotPrefixed(iAdmin, GetTranslation("AdminFunctions_CantFindTarget"));
        return;
    }

    time_t now = std::time(nullptr);
    int64_t iCreatedAt = (int64_t)now;
    int64_t iExpireAt = iDuration == 0 ? 0 : (iCreatedAt + (int64_t)iDuration);
    
    char query[512];
    g_SMAPI->Format(query, sizeof(query), 
        "INSERT INTO jb_punishments (sid64, adminsid, created_at, expires_at, reason) "
        "VALUES (%llu, %llu, %lld, %lld, '%s') "
        "ON DUPLICATE KEY UPDATE created_at = VALUES(created_at), expires_at = VALUES(expires_at), reason = VALUES(reason);",
        targetSid, iAdminSID, iCreatedAt, iExpireAt, connection->Escape(reason.c_str()).c_str()
    );

    std::string sAdminName = szAdminName;

    if (!connection) return;
    
    connection->Query(query, [bConsole, iTarget, iAdmin, iDuration, reason, sAdminName, sTargetName, targetSid](ISQLQuery* res) {
        if (res && res->GetAffectedRows() > 0) {
            char msg[256];
            

            if (iTarget != -1) {
                g_SMAPI->Format(msg, sizeof(msg), GetTranslation("AdminFunctions_YouGotBanned"), sAdminName.c_str(), reason.c_str(), TimeConverter(iDuration).c_str());
                PrintSlotPrefixed(iTarget, msg);

                auto pController = CCSPlayerController::FromSlot(iTarget);
                if (pController && pController->m_steamID == targetSid) {
                    if (pController->GetTeam() == 3) {
                        players_api->ChangeTeam(iTarget, 2);
                    }
                }
            }

            if (!bConsole) {
                g_SMAPI->Format(msg, sizeof(msg), GetTranslation("AdminFunctions_YouBannedPlayer"), sTargetName.c_str(), reason.c_str(), TimeConverter(iDuration).c_str());
                PrintSlotPrefixed(iAdmin, msg);
            } else {
                META_CONPRINTF("[Jailbreak] Banned %s for %s. Reason: %s\n", sTargetName.c_str(), reason.c_str(), TimeConverter(iDuration).c_str());
            }
        }
    });
}


void HandleBanCommand(int iAdmin, const CCommand &args) {\
    bool bConsole = (iAdmin == -1);

    auto PrintUsage = [iAdmin, bConsole]() {
        if (bConsole) {
            META_CONPRINTF("[Jailbreak] Usage: mm_banct <userid | steamid64> <duration> <reason>\n");
        } else {
            utils->PrintToConsole(iAdmin, "[Jailbreak] Usage: mm_banct <userid | steamid64> <duration> <reason>\n");
            PrintSlotPrefixed(iAdmin, "Usage: !banct <userid | steamid64> <duration> <reason>");
        }
    };

    if (!bConsole && !admin_api->HasPermission(iAdmin, sBanPermission.c_str())) {
        PrintSlotPrefixed(iAdmin, GetTranslation("AdminFunctions_NoPermission"));
        return;
    }

    

    if (args.ArgC() < 4) {
        PrintUsage();
        return; 
    }
    if (!OnlyDigits(args.Arg(1))){
        PrintUsage();
        return; 
    }
    if (!OnlyDigits(args.Arg(2))){
        PrintUsage();
        return; 
    }
    std::string arg1 = args.Arg(1);
    int iDuration = atoi(args.Arg(2));

    std::string reasonStr = "";
    for (int i = 3; i < args.ArgC(); ++i) {
        reasonStr += args.Arg(i);
        if (i < args.ArgC() - 1) {
            reasonStr += " ";
        }
    }

    BanPlayer(iAdmin, arg1, iDuration, reasonStr);
}

void BanConsoleCommand(const CCommandContext &context, const CCommand &args) {
    HandleBanCommand(context.GetPlayerSlot(), args);
}

CON_COMMAND_EXTERN_F(mm_banct, BanConsoleCommand, "Ban player possibility join CT command",FCVAR_CLIENT_CAN_EXECUTE);

bool BanChatCommand(int iAdmin, const char* content) {
    CCommand args;
    args.Tokenize(content);
    HandleBanCommand(iAdmin, args);
    return false;
}

// ===================================
// KICK CT COMMAND
// ===================================

void HandleKickCommand(int iAdmin, const CCommand &args) {\
    bool bConsole = (iAdmin == -1);

    auto PrintUsage = [iAdmin, bConsole]() {
        if (bConsole) {
            META_CONPRINTF("[Jailbreak] Usage: mm_kickct <userid | steamid64>\n");
        } else {
            utils->PrintToConsole(iAdmin, "[Jailbreak] Usage: mm_kickct <userid | steamid64>\n");
            PrintSlotPrefixed(iAdmin, "Usage: !kickct <userid | steamid64>");
        }
    };

    if (!bConsole && !admin_api->HasPermission(iAdmin, sKickCTPermission.c_str())) {
        PrintSlotPrefixed(iAdmin, GetTranslation("AdminFunctions_NoPermission"));
        return;
    }

    if (args.ArgC() < 3) {
        PrintUsage();
        return; 
    }
    if (!OnlyDigits(args.Arg(1))){
        PrintUsage();
        return; 
    }

    std::string arg1 = args.Arg(1);

    bool bFound = false;
    int iTarget = -1;

    bool isDigits = OnlyDigits(arg1.c_str());
    int targetSlot = -1;
    uint64_t targetSid = 0;

    if (isDigits) {
        if (arg1.length() <= 2) {
            targetSlot = atoi(arg1.c_str());
        } else if (arg1.length() == 17) { 
            targetSid = std::stoull(arg1);
        }
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players_api->IsFakeClient(i)) continue;
        
        auto controller = CCSPlayerController::FromSlot(i);
        if (!controller || controller->m_steamID == 0) continue;

        if (targetSlot != -1 && i == targetSlot) {
            iTarget = i;
            bFound = true;
            break;
        } 
        else if (targetSid != 0 && controller->m_steamID == targetSid) {
            iTarget = i;
            bFound = true;
            break;
        }
    }
    if (iTarget == -1) return;
    auto cTarget = CCSPlayerController::FromSlot(iTarget);
    if (!cTarget || cTarget->GetTeam() != 3) {
        PrintSlotPrefixed(iAdmin,GetTranslation("AdminFunctions_PlayerNotCT"));
        return;
    }
    
    std::string sAdminName;
    if (bConsole) {
        sAdminName = "Console";
    } else {
        auto aTarget = CCSPlayerController::FromSlot(iAdmin);
        if (aTarget) sAdminName = aTarget->GetPlayerName();
        else sAdminName = "Console";
    }



    players_api->ChangeTeam(iTarget,2);

    char msg[256];
    g_SMAPI->Format(msg,sizeof(msg),GetTranslation("AdminFunctions_KickedYouFromCT"),sAdminName.c_str());
    PrintSlotPrefixed(iTarget,msg);
    g_SMAPI->Format(msg,sizeof(msg),GetTranslation("AdminFunctions_YouAdminKickFromCt"),cTarget->GetPlayerName());
    PrintSlotPrefixed(iAdmin,msg);
}

void KickConsoleCommand(const CCommandContext &context, const CCommand &args) {
    HandleKickCommand(context.GetPlayerSlot(), args);
}

CON_COMMAND_EXTERN_F(mm_kickct, KickConsoleCommand, "Kick player from CT team",FCVAR_CLIENT_CAN_EXECUTE);

bool KickChatCommand(int iAdmin, const char* content) {
    CCommand args;
    args.Tokenize(content);
    HandleKickCommand(iAdmin, args);
    return false;
}

// =========================================
// CHECK BAN
// =========================================

void HandleCheckBanCommand(int iAdmin, const CCommand &args) {
    bool bConsole = iAdmin == -1;

    auto PrintUsage = [iAdmin, bConsole]() {
        if (bConsole) {
            META_CONPRINTF("[Jailbreak] Usage: mm_checkban_ct <steamid64>\n");
        } else {
            utils->PrintToConsole(iAdmin, "[Jailbreak] Usage: mm_checkban_ct <steamid64>\n");
            PrintSlotPrefixed(iAdmin, "Usage: !checkban_ct <steamid64>");
        }
    };

    if (!bConsole && !admin_api->HasPermission(iAdmin, sCheckCTBanPermission.c_str())) {
        PrintSlotPrefixed(iAdmin, GetTranslation("AdminFunctions_NoPermission"));
        return;
    }

    if (args.ArgC() < 2) {
        PrintUsage();
        return; 
    }

    std::string arg1 = args.Arg(1);
    if (!OnlyDigits(arg1.c_str())) {
        PrintUsage();
        return;
    }

    uint64_t iTargetSID = 0;
    try {
        iTargetSID = std::stoull(arg1);
    } catch (...) {
        PrintUsage();
        return;
    }

    time_t now = std::time(nullptr);
    int64_t iCurrentTime = (int64_t)now;
    
    char query[512];
    g_SMAPI->Format(query, sizeof(query), 
        "SELECT adminsid, reason, expires_at FROM jb_punishments WHERE sid64 = %llu AND (expires_at = 0 OR expires_at > %lld);",
        iTargetSID, iCurrentTime);

    if (connection) {
        connection->Query(query, [iTargetSID, arg1, iAdmin, bConsole, iCurrentTime](ISQLQuery* res) {
            
            bool bIsBanned = false;
            std::string sAdminSID = "0";
            std::string sReason = "No reason";
            uint64_t iDurationLeft = 0;

            if (res) {
                auto result = res->GetResultSet();
                if (result && result->GetRowCount() > 0 && result->FetchRow()) {
                    bIsBanned = true;

                    const char* szAdminSID = result->GetString(0);
                    if (szAdminSID) sAdminSID = szAdminSID;

                    const char* szReason = result->GetString(1);
                    if (szReason) sReason = szReason;

                    const char* szExpAt = result->GetString(2);
                    uint64_t iExpAt = 0;
                    
                    if (szExpAt && szExpAt[0] != '\0') {
                        iExpAt = std::strtoull(szExpAt, nullptr, 10);
                    }

                    if (iExpAt > 0 && iExpAt > iCurrentTime) {
                        iDurationLeft = iExpAt - iCurrentTime;
                    }
                }
            }

            std::string sTimeLeft = TimeConverter(iDurationLeft);
            if (bIsBanned) {
                if (bConsole) {
                    META_CONPRINTF("[Jailbreak] Player %llu banned by %s. Reason: %s. Time before unban: %s\n",
                                    iTargetSID, sAdminSID.c_str(), sReason.c_str(), sTimeLeft.c_str());
                } else {
                    char msg[256];

                    g_SMAPI->Format(msg, sizeof(msg),
                        GetTranslation("AdminFunctions_CheckBanInfo"),
                        arg1.c_str(), 
                        sAdminSID.c_str(),
                        sReason.c_str(),
                        sTimeLeft.c_str()
                    );

                    PrintSlotPrefixed(iAdmin, msg);
                    utils->PrintToConsole(iAdmin, "[Jailbreak] Player %llu banned by %s. Reason: %s. Time before unban: %s",
                                            iTargetSID, sAdminSID.c_str(), sReason.c_str(), sTimeLeft.c_str());
                }
            } else {
                if (bConsole) {
                    META_CONPRINTF("[Jailbreak] Player %llu not banned.\n", iTargetSID);
                } else {
                    char msg[256];
                    g_SMAPI->Format(msg, sizeof(msg),
                        GetTranslation("AdminFunctions_CheckBanNotBanned"), iTargetSID);

                    PrintSlotPrefixed(iAdmin, msg);
                    utils->PrintToConsole(iAdmin, "[Jailbreak] Player %llu not banned.", iTargetSID);
                }
            }
        });
    }
}

void CheckBanConsoleCommand(const CCommandContext &context, const CCommand &args) {
    HandleCheckBanCommand(context.GetPlayerSlot(), args);
}

CON_COMMAND_EXTERN_F(mm_checkban_ct, CheckBanConsoleCommand, "Instantly transfer player to CT team",FCVAR_CLIENT_CAN_EXECUTE);

bool CheckBanChatCommand(int iAdmin, const char* content) {
    CCommand args;
    args.Tokenize(content);
    HandleCheckBanCommand(iAdmin, args);
    return false;
}

// =========================================
// UNBAN CT
// =========================================

void HandleUnBanCommand(int iAdmin, const CCommand &args) {
    bool bConsole = iAdmin == -1;

    auto PrintUsage = [iAdmin, bConsole]() {
        if (bConsole) {
            META_CONPRINTF("[Jailbreak] Usage: mm_unban_ct <steamid64>\n");
        } else {
            utils->PrintToConsole(iAdmin, "[Jailbreak] Usage: mm_unban_ct <steamid64>\n");
            PrintSlotPrefixed(iAdmin, "Usage: !unban_ct <steamid64>");
        }
    };

    if (!bConsole && !admin_api->HasPermission(iAdmin, sUnbanCTPermission.c_str())) {
        PrintSlotPrefixed(iAdmin, GetTranslation("AdminFunctions_NoPermission"));
        return;
    }

    if (args.ArgC() < 2) {
        PrintUsage();
        return; 
    }

    std::string arg1 = args.Arg(1);
    if (!OnlyDigits(arg1.c_str())) {
        PrintUsage();
        return;
    }

    uint64_t iTargetSID = 0;
    try {
        iTargetSID = std::stoull(arg1);
    } catch (...) {
        PrintUsage();
        return;
    }
    
    char query[512];
    g_SMAPI->Format(query, sizeof(query), "DELETE FROM jb_punishments WHERE sid64 = %llu;", iTargetSID);

    if (connection) {
        connection->Query(query, [iTargetSID, iAdmin, bConsole](ISQLQuery* res) {
            if (res && res->GetAffectedRows() > 0) {
                if (bConsole) {
                    META_CONPRINTF("[Jailbreak] Player %llu successfully unbanned.\n", iTargetSID);
                } else {
                    char msg[256];
                    g_SMAPI->Format(msg, sizeof(msg), GetTranslation("AdminFunctions_PlayerUnbanSuccess"), iTargetSID);
                    PrintSlotPrefixed(iAdmin, msg);
                    utils->PrintToConsole(iAdmin, "[Jailbreak] Player %llu successfully unbanned.\n", iTargetSID);
                }
            } else {
                if (bConsole) {
                    META_CONPRINTF("[Jailbreak] Player %llu is not banned.\n", iTargetSID);
                } else {
                    char msg[256];
                    g_SMAPI->Format(msg, sizeof(msg), GetTranslation("AdminFunctions_PlayerNotBanned"), iTargetSID);
                    PrintSlotPrefixed(iAdmin, msg);
                    utils->PrintToConsole(iAdmin, "[Jailbreak] Player %llu is not banned.\n", iTargetSID);
                }
            }
        });
    }
}

void UnBanConsoleCommand(const CCommandContext &context, const CCommand &args) {
    HandleUnBanCommand(context.GetPlayerSlot(), args);
}

CON_COMMAND_EXTERN_F(mm_unban_ct, UnBanConsoleCommand, "Allow player join CT if he had ban",FCVAR_CLIENT_CAN_EXECUTE);

bool UnbanChatCommand(int iAdmin, const char* content) {
    CCommand args;
    args.Tokenize(content);
    HandleUnBanCommand(iAdmin, args);
    return false;
}

// =========================================
// OTHER
// =========================================

CGameEntitySystem* GameEntitySystem() {
    return utils ? utils->GetCGameEntitySystem() : nullptr;
}

void StartupServer() {
    g_pGameEntitySystem = GameEntitySystem();
    g_pEntitySystem = utils->GetCEntitySystem();
    gpGlobals = utils->GetCGlobalVars();
}

bool jb_admin_functions::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) {
    PLUGIN_SAVEVARS();

    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkSystem, INetworkSystem, NETWORKSYSTEM_INTERFACE_VERSION);

    ConVar_Register(FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);
    g_SMAPI->AddListener(this, this);

    return true;
}



void jb_admin_functions::AllPluginsLoaded() {
    int ret;
    utils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    players_api = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing UTILS plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    jailbreak_api =(IJailbreakApi*)g_SMAPI->MetaFactory(JAILBREAK_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Jailbreak Core plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    ISQLInterface* sql_interface = (ISQLInterface*)g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Mysql plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    admin_api = (IAdminApi*)g_SMAPI->MetaFactory(Admin_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) {
        META_CONPRINTF("%s | Missing Admin System plugin.",g_PLAPI->GetLogTag());
        engine->ServerCommand(("meta unload " + std::to_string(g_PLID)).c_str());
        return;
    }

    mysql_client = sql_interface->GetMySQLClient();

    LoadDatabase();
    LoadTranslations();

    utils->RegCommand(g_PLID,{},{"!banct"},BanChatCommand);
    utils->RegCommand(g_PLID,{},{"!kickct"},KickChatCommand);
    utils->RegCommand(g_PLID,{},{"!checkban_ct"},CheckBanChatCommand);
    utils->RegCommand(g_PLID, {},{"!unban_ct"}, UnbanChatCommand);


    utils->StartupServer(g_PLID, StartupServer);

}

bool jb_admin_functions::Unload(char* error, size_t maxlen) {
    jailbreak_api->ClearAllPluginHooks(g_PLID);
    utils->ClearAllHooks(g_PLID);
    ConVar_Unregister();

   
    return true;
}

const char* jb_admin_functions::GetAuthor() { return "niffox"; }
const char* jb_admin_functions::GetDate() { return __DATE__; }
const char* jb_admin_functions::GetDescription() { return "[JB] Admin Functions"; }
const char* jb_admin_functions::GetLicense() { return "Private"; }
const char* jb_admin_functions::GetLogTag() { return "[JB] Admin Functions"; }
const char* jb_admin_functions::GetName() { return "[JB] Admin Functions"; }
const char* jb_admin_functions::GetURL() { return "https://t.me/niffox_2q"; }
const char* jb_admin_functions::GetVersion() { return "1.0.1"; }