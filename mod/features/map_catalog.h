#pragma once
// Auto-derived from config/SC6MissionData.xml and config/Checkpoints.ini
// Map IDs match the game's internal map name (set as g_config.mapName in the level editor).
// Use these to populate the map selector dropdown and to cross-validate level files.

namespace MapCatalog {

struct MapEntry {
    const char* id;          // Internal game map name (e.g. "S_AFB")
    const char* displayName; // Human-readable mission title (en-US)
    const char* category;    // "SP", "Coop", "SideMission", "Adversarial", "Multiplayer"
    const char* mapFile;     // .unr stream file path fragment (relative to game data dir)
};

// Single-player missions (in story order)
// File paths: _Single\{ID}\Stream\{ID}_Global_00_strm.unr
static constexpr MapEntry kMaps[] = {
    // SP Campaign (Sam)
    { "S_AFB", "Blacklist Zero",        "SP",          "_Single\\S_AFB\\Stream\\S_AFB_Global_00_strm.unr" },
    { "S_KOB", "Safehouse",             "SP",          "_Single\\S_KOB\\Stream\\S_KOB_Global_00_strm.unr" },
    { "S_BAD", "Insurgent Stronghold",  "SP",          "_Single\\S_BAD\\Stream\\S_BAD_Global_00_strm.unr" },
    { "S_CON", "American Consumption",  "SP",          "_Single\\S_CON\\Stream\\S_CON_Global_00_strm.unr" },
    { "S_NOU", "Private Estate",        "SP",          "_Single\\S_NOU\\Stream\\S_NOU_Global_00_strm.unr" },
    { "S_CHE", "Abandoned Mill",        "SP",          "_Single\\S_CHE\\Stream\\S_CHE_Global_00_strm.unr" },
    { "S_QOD", "Special Missions HQ",   "SP",          "_Single\\S_QOD\\Stream\\S_QOD_Global_00_strm.unr" },
    { "S_FRE", "Transit Yards",         "SP",          "_Single\\S_FRE\\Stream\\S_FRE_Global_00_strm.unr" },
    { "S_DET", "Detention Facility",    "SP",          "_Single\\S_DET\\Stream\\S_DET_Global_00_strm.unr" },
    { "S_AIR", "Airstrip",              "SP",          "_Single\\S_AIR\\Stream\\S_AIR_Global_00_strm.unr" },
    { "S_PAL", "American Fuel",         "SP",          "_Single\\S_PAL\\Stream\\S_PAL_Global_00_strm.unr" },
    { "S_FUE", "LNG Terminal",          "SP",          "_Single\\S_FUE\\Stream\\S_FUE_Global_00_strm.unr" },
    { "S_BLO", "Site F",                "SP",          "_Single\\S_BLO\\Stream\\S_BLO_Global_00_strm.unr" },
    // Coop (Briggs)
    { "C01",   "Smugglers Compound",    "Coop",        "_COOP\\C01_MafiaClub.unr" },
    { "C02",   "Missile Plant",         "Coop",        "_COOP\\C02.unr" },
    { "C03",   "VORON Station",         "Coop",        "_COOP\\C03.unr" },
    { "C04",   "Abandoned City",        "Coop",        "_COOP\\C04.unr" },
    // Side ops (Grim)
    { "G01",   "Hawkins Seafort",       "SideMission", "" },
    { "G02",   "Border Crossing",       "SideMission", "" },
    { "G03",   "Hackers' Den",          "SideMission", "" },
    { "G04",   "Billionaire's Yacht",   "SideMission", "" },
    // Side ops (Charlie/Echelon)
    { "E01",   "Pakistani Embassy",     "SideMission", "" },
    { "E02",   "Swiss Embassy",         "SideMission", "" },
    { "E03",   "Egyptian Embassy",      "SideMission", "" },
    { "E04",   "Russian Embassy",       "SideMission", "" },
    // Side ops (Kobin)
    { "H01",   "Opium Farm",            "SideMission", "" },
    { "H02",   "Fish Market",           "SideMission", "" },
    { "H03",   "Blood Diamond Mine",    "SideMission", "" },
    { "H04",   "Dead Coast",            "SideMission", "" },
    // Adversarial
    { "A01",   "Virus Vault",           "Adversarial", "" },
    // Multiplayer Extraction/Deniable Ops maps (from Maps/StatsMaps.ini)
    { "D_DiamondMine", "Diamond Mine",  "Multiplayer", "_MULTI\\D_DiamondMine_Global.unr" },
    { "D_FishMarket",  "Fish Market",   "Multiplayer", "_MULTI\\D_FishMarket_Global.unr" },
    { "D_OpiumFarm",   "Opium Farm",    "Multiplayer", "_MULTI\\D_OpiumFarm_Global.unr" },
    { "D_BorderCross", "Border Cross",  "Multiplayer", "_MULTI\\D_BorderCross_Global.unr" },
    { "D_HackerDen",   "Hacker Den",   "Multiplayer", "_MULTI\\D_HackerDen_Global.unr" },
    { "D_SeaFort",     "Sea Fort",      "Multiplayer", "_MULTI\\D_SeaFort_Global.unr" },
    { "D_Yacht",       "Yacht",         "Multiplayer", "_MULTI\\D_Yacht_Global.unr" },
    { "D_Amman",       "Amman",         "Multiplayer", "_MULTI\\D_Amman_Global.unr" },
    { "D_Bratislava",  "Bratislava",    "Multiplayer", "_MULTI\\D_Bratislava_Global.unr" },
    { "D_Kigali",      "Kigali",        "Multiplayer", "_MULTI\\D_Kigali_Global.unr" },
    { "D_Sanaa",       "Sanaa",         "Multiplayer", "_MULTI\\D_Sanaa_Global.unr" },
    // HUB
    { "HUB",   "Paladin (Hub)",         "SP",          "HUB_Global.unr" },
    { "Menu",  "Main Menu",             "SP",          "menu.unr" },
};

static constexpr int kMapCount = static_cast<int>(sizeof(kMaps) / sizeof(kMaps[0]));

// Returns nullptr if not found.
inline const MapEntry* FindById(const char* id) {
    for (int i = 0; i < kMapCount; ++i)
        if (strcmp(kMaps[i].id, id) == 0) return &kMaps[i];
    return nullptr;
}

inline const char* DisplayName(const char* id) {
    const MapEntry* e = FindById(id);
    return e ? e->displayName : id;
}

} // namespace MapCatalog
