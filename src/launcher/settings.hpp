#pragma once
#include "launcher/model.hpp"
#include "launcher/shaders.hpp"
#include "launcher/view_motion.hpp"
#include <array>
#include <charconv>

namespace rgds {
enum class Page {
    Library,
    Settings,
    Appearance,
    Placeholder,
    Power,
    Display,
    LibraryOptions,
    Scraper,
    Keyboard,
    System,
    Timezone,
    Sound,
    QuickMenu,
    LeaveGame,
    Network,
    Wifi,
    DeviceInfo,
    Maintenance,
    Confirm,
    GameOptions,
    ShaderPicker,
    EmulatorOptions,
    ApplyShader,
    BootWarning,
    Startup,
    Controls,
    GameArtwork,
    Statistics,
    SaveGallery,
    Discover, // retired: Search and filters moved into the Library view sheet; the slot keeps stored page numbers
    RomSetup,
    DSControls,
    Microphone,
    Motion,
    MenuSounds,
    Graphics,
    GameSwitcher,
    Colours,
    Cheats,
    // Appended: page numbers are stored in the launcher's state file.
    PlayGraphics, // in-game Graphics for RG DS Play: game screen fit, second screen
    GameInfo,     // the selected game's facts, description and pictures (read-only)
    Haptics,      // Sound -> Vibration: menu haptics and game rumble, each with a strength
    PortFiles,    // Add game files: a port's own files, sent from a phone or computer (port_files_screen.hpp)
    DisplayOptions // Settings > Display: brightness, animations, touch indicator, home layout, touch gestures
};
struct Preferences {
    bool dark = false, pixel = false, auto_load = true, auto_save = true, animations = true, view_animations = false,
         touch_indicator = true, rich_icons = true, menu_sounds = true, high_contrast = false;
    int accent = -1;
    ViewStyle view_style = ViewStyle::Spring;
    int view_speed = 1;
    std::string language = "en-US";
    unsigned core = 4;
    int filter = 0, view = 0, layout = 0, artwork_provider = 0, drastic_backend = 0, sound_volume = 55;
    int home_layout = 1, background = 1;
    bool gestures = false;
    bool show_systems = false; // Library: a Systems step between Home and the games (ui.ini)
    bool menu_music = true;
    int music_volume = 40;
    // The vibration motor: patterns in the menus, rumble in games that have it
    // (DS Rumble Pak, GB rumble carts, GBA rumble). Strengths are 10-100%.
    bool menu_haptics = true, game_rumble = true;
    int haptic_strength = 60, rumble_strength = 80;
    // Artwork sources: what "Fetch missing artwork" also downloads. Game info is the text from the public
    // databases; extra_pictures is a bit set (1 screenshot, 2 title screen). Both only add to the library.
    bool game_info = true;
    int extra_pictures = 2;
};
struct SystemPreferences {
    int power = 0, brightness = 60, idle = 0;
    bool lid = true, battery_percent = true;
    std::string timezone = "Europe/Oslo";
    int dst = 0; // daylight saving: 0 follows the zone's rules, 1 forced on, 2 forced off (standard time)
    int date_format = 0;
    int time_format = 0; // 0: 24-hour, 1: 12-hour
};
inline const std::array<const char*, 3> dst_names = {"Automatic", "On", "Off"};
inline const std::array<const char*, 2> time_format_names = {"24-hour", "12-hour"};
inline const std::array<const char*, 5> date_format_names = {"DD/MM", "MM/DD", "DD.MM", "YYYY-MM-DD", "DD-MM-YYYY"};
inline const std::array<const char*, 5> date_format_patterns = {"%d/%m", "%m/%d", "%d.%m", "%Y-%m-%d", "%d-%m-%Y"};
// Every zone in the device's tzdata zone.tab (one per country and region,
// so Europe/Oslo stays a zone of its own), sorted, with UTC first.
inline const std::array<const char*, 419> timezones = {"UTC",
                                                       "Africa/Abidjan",
                                                       "Africa/Accra",
                                                       "Africa/Addis_Ababa",
                                                       "Africa/Algiers",
                                                       "Africa/Asmara",
                                                       "Africa/Bamako",
                                                       "Africa/Bangui",
                                                       "Africa/Banjul",
                                                       "Africa/Bissau",
                                                       "Africa/Blantyre",
                                                       "Africa/Brazzaville",
                                                       "Africa/Bujumbura",
                                                       "Africa/Cairo",
                                                       "Africa/Casablanca",
                                                       "Africa/Ceuta",
                                                       "Africa/Conakry",
                                                       "Africa/Dakar",
                                                       "Africa/Dar_es_Salaam",
                                                       "Africa/Djibouti",
                                                       "Africa/Douala",
                                                       "Africa/El_Aaiun",
                                                       "Africa/Freetown",
                                                       "Africa/Gaborone",
                                                       "Africa/Harare",
                                                       "Africa/Johannesburg",
                                                       "Africa/Juba",
                                                       "Africa/Kampala",
                                                       "Africa/Khartoum",
                                                       "Africa/Kigali",
                                                       "Africa/Kinshasa",
                                                       "Africa/Lagos",
                                                       "Africa/Libreville",
                                                       "Africa/Lome",
                                                       "Africa/Luanda",
                                                       "Africa/Lubumbashi",
                                                       "Africa/Lusaka",
                                                       "Africa/Malabo",
                                                       "Africa/Maputo",
                                                       "Africa/Maseru",
                                                       "Africa/Mbabane",
                                                       "Africa/Mogadishu",
                                                       "Africa/Monrovia",
                                                       "Africa/Nairobi",
                                                       "Africa/Ndjamena",
                                                       "Africa/Niamey",
                                                       "Africa/Nouakchott",
                                                       "Africa/Ouagadougou",
                                                       "Africa/Porto-Novo",
                                                       "Africa/Sao_Tome",
                                                       "Africa/Tripoli",
                                                       "Africa/Tunis",
                                                       "Africa/Windhoek",
                                                       "America/Adak",
                                                       "America/Anchorage",
                                                       "America/Anguilla",
                                                       "America/Antigua",
                                                       "America/Araguaina",
                                                       "America/Argentina/Buenos_Aires",
                                                       "America/Argentina/Catamarca",
                                                       "America/Argentina/Cordoba",
                                                       "America/Argentina/Jujuy",
                                                       "America/Argentina/La_Rioja",
                                                       "America/Argentina/Mendoza",
                                                       "America/Argentina/Rio_Gallegos",
                                                       "America/Argentina/Salta",
                                                       "America/Argentina/San_Juan",
                                                       "America/Argentina/San_Luis",
                                                       "America/Argentina/Tucuman",
                                                       "America/Argentina/Ushuaia",
                                                       "America/Aruba",
                                                       "America/Asuncion",
                                                       "America/Atikokan",
                                                       "America/Bahia",
                                                       "America/Bahia_Banderas",
                                                       "America/Barbados",
                                                       "America/Belem",
                                                       "America/Belize",
                                                       "America/Blanc-Sablon",
                                                       "America/Boa_Vista",
                                                       "America/Bogota",
                                                       "America/Boise",
                                                       "America/Cambridge_Bay",
                                                       "America/Campo_Grande",
                                                       "America/Cancun",
                                                       "America/Caracas",
                                                       "America/Cayenne",
                                                       "America/Cayman",
                                                       "America/Chicago",
                                                       "America/Chihuahua",
                                                       "America/Ciudad_Juarez",
                                                       "America/Costa_Rica",
                                                       "America/Creston",
                                                       "America/Cuiaba",
                                                       "America/Curacao",
                                                       "America/Danmarkshavn",
                                                       "America/Dawson",
                                                       "America/Dawson_Creek",
                                                       "America/Denver",
                                                       "America/Detroit",
                                                       "America/Dominica",
                                                       "America/Edmonton",
                                                       "America/Eirunepe",
                                                       "America/El_Salvador",
                                                       "America/Fort_Nelson",
                                                       "America/Fortaleza",
                                                       "America/Glace_Bay",
                                                       "America/Goose_Bay",
                                                       "America/Grand_Turk",
                                                       "America/Grenada",
                                                       "America/Guadeloupe",
                                                       "America/Guatemala",
                                                       "America/Guayaquil",
                                                       "America/Guyana",
                                                       "America/Halifax",
                                                       "America/Havana",
                                                       "America/Hermosillo",
                                                       "America/Indiana/Indianapolis",
                                                       "America/Indiana/Knox",
                                                       "America/Indiana/Marengo",
                                                       "America/Indiana/Petersburg",
                                                       "America/Indiana/Tell_City",
                                                       "America/Indiana/Vevay",
                                                       "America/Indiana/Vincennes",
                                                       "America/Indiana/Winamac",
                                                       "America/Inuvik",
                                                       "America/Iqaluit",
                                                       "America/Jamaica",
                                                       "America/Juneau",
                                                       "America/Kentucky/Louisville",
                                                       "America/Kentucky/Monticello",
                                                       "America/Kralendijk",
                                                       "America/La_Paz",
                                                       "America/Lima",
                                                       "America/Los_Angeles",
                                                       "America/Lower_Princes",
                                                       "America/Maceio",
                                                       "America/Managua",
                                                       "America/Manaus",
                                                       "America/Marigot",
                                                       "America/Martinique",
                                                       "America/Matamoros",
                                                       "America/Mazatlan",
                                                       "America/Menominee",
                                                       "America/Merida",
                                                       "America/Metlakatla",
                                                       "America/Mexico_City",
                                                       "America/Miquelon",
                                                       "America/Moncton",
                                                       "America/Monterrey",
                                                       "America/Montevideo",
                                                       "America/Montserrat",
                                                       "America/Nassau",
                                                       "America/New_York",
                                                       "America/Nome",
                                                       "America/Noronha",
                                                       "America/North_Dakota/Beulah",
                                                       "America/North_Dakota/Center",
                                                       "America/North_Dakota/New_Salem",
                                                       "America/Nuuk",
                                                       "America/Ojinaga",
                                                       "America/Panama",
                                                       "America/Paramaribo",
                                                       "America/Phoenix",
                                                       "America/Port-au-Prince",
                                                       "America/Port_of_Spain",
                                                       "America/Porto_Velho",
                                                       "America/Puerto_Rico",
                                                       "America/Punta_Arenas",
                                                       "America/Rankin_Inlet",
                                                       "America/Recife",
                                                       "America/Regina",
                                                       "America/Resolute",
                                                       "America/Rio_Branco",
                                                       "America/Santarem",
                                                       "America/Santiago",
                                                       "America/Santo_Domingo",
                                                       "America/Sao_Paulo",
                                                       "America/Scoresbysund",
                                                       "America/Sitka",
                                                       "America/St_Barthelemy",
                                                       "America/St_Johns",
                                                       "America/St_Kitts",
                                                       "America/St_Lucia",
                                                       "America/St_Thomas",
                                                       "America/St_Vincent",
                                                       "America/Swift_Current",
                                                       "America/Tegucigalpa",
                                                       "America/Thule",
                                                       "America/Tijuana",
                                                       "America/Toronto",
                                                       "America/Tortola",
                                                       "America/Vancouver",
                                                       "America/Whitehorse",
                                                       "America/Winnipeg",
                                                       "America/Yakutat",
                                                       "Antarctica/Casey",
                                                       "Antarctica/Davis",
                                                       "Antarctica/DumontDUrville",
                                                       "Antarctica/Macquarie",
                                                       "Antarctica/Mawson",
                                                       "Antarctica/McMurdo",
                                                       "Antarctica/Palmer",
                                                       "Antarctica/Rothera",
                                                       "Antarctica/Syowa",
                                                       "Antarctica/Troll",
                                                       "Antarctica/Vostok",
                                                       "Arctic/Longyearbyen",
                                                       "Asia/Aden",
                                                       "Asia/Almaty",
                                                       "Asia/Amman",
                                                       "Asia/Anadyr",
                                                       "Asia/Aqtau",
                                                       "Asia/Aqtobe",
                                                       "Asia/Ashgabat",
                                                       "Asia/Atyrau",
                                                       "Asia/Baghdad",
                                                       "Asia/Bahrain",
                                                       "Asia/Baku",
                                                       "Asia/Bangkok",
                                                       "Asia/Barnaul",
                                                       "Asia/Beirut",
                                                       "Asia/Bishkek",
                                                       "Asia/Brunei",
                                                       "Asia/Chita",
                                                       "Asia/Choibalsan",
                                                       "Asia/Colombo",
                                                       "Asia/Damascus",
                                                       "Asia/Dhaka",
                                                       "Asia/Dili",
                                                       "Asia/Dubai",
                                                       "Asia/Dushanbe",
                                                       "Asia/Famagusta",
                                                       "Asia/Gaza",
                                                       "Asia/Hebron",
                                                       "Asia/Ho_Chi_Minh",
                                                       "Asia/Hong_Kong",
                                                       "Asia/Hovd",
                                                       "Asia/Irkutsk",
                                                       "Asia/Jakarta",
                                                       "Asia/Jayapura",
                                                       "Asia/Jerusalem",
                                                       "Asia/Kabul",
                                                       "Asia/Kamchatka",
                                                       "Asia/Karachi",
                                                       "Asia/Kathmandu",
                                                       "Asia/Khandyga",
                                                       "Asia/Kolkata",
                                                       "Asia/Krasnoyarsk",
                                                       "Asia/Kuala_Lumpur",
                                                       "Asia/Kuching",
                                                       "Asia/Kuwait",
                                                       "Asia/Macau",
                                                       "Asia/Magadan",
                                                       "Asia/Makassar",
                                                       "Asia/Manila",
                                                       "Asia/Muscat",
                                                       "Asia/Nicosia",
                                                       "Asia/Novokuznetsk",
                                                       "Asia/Novosibirsk",
                                                       "Asia/Omsk",
                                                       "Asia/Oral",
                                                       "Asia/Phnom_Penh",
                                                       "Asia/Pontianak",
                                                       "Asia/Pyongyang",
                                                       "Asia/Qatar",
                                                       "Asia/Qostanay",
                                                       "Asia/Qyzylorda",
                                                       "Asia/Riyadh",
                                                       "Asia/Sakhalin",
                                                       "Asia/Samarkand",
                                                       "Asia/Seoul",
                                                       "Asia/Shanghai",
                                                       "Asia/Singapore",
                                                       "Asia/Srednekolymsk",
                                                       "Asia/Taipei",
                                                       "Asia/Tashkent",
                                                       "Asia/Tbilisi",
                                                       "Asia/Tehran",
                                                       "Asia/Thimphu",
                                                       "Asia/Tokyo",
                                                       "Asia/Tomsk",
                                                       "Asia/Ulaanbaatar",
                                                       "Asia/Urumqi",
                                                       "Asia/Ust-Nera",
                                                       "Asia/Vientiane",
                                                       "Asia/Vladivostok",
                                                       "Asia/Yakutsk",
                                                       "Asia/Yangon",
                                                       "Asia/Yekaterinburg",
                                                       "Asia/Yerevan",
                                                       "Atlantic/Azores",
                                                       "Atlantic/Bermuda",
                                                       "Atlantic/Canary",
                                                       "Atlantic/Cape_Verde",
                                                       "Atlantic/Faroe",
                                                       "Atlantic/Madeira",
                                                       "Atlantic/Reykjavik",
                                                       "Atlantic/South_Georgia",
                                                       "Atlantic/St_Helena",
                                                       "Atlantic/Stanley",
                                                       "Australia/Adelaide",
                                                       "Australia/Brisbane",
                                                       "Australia/Broken_Hill",
                                                       "Australia/Darwin",
                                                       "Australia/Eucla",
                                                       "Australia/Hobart",
                                                       "Australia/Lindeman",
                                                       "Australia/Lord_Howe",
                                                       "Australia/Melbourne",
                                                       "Australia/Perth",
                                                       "Australia/Sydney",
                                                       "Europe/Amsterdam",
                                                       "Europe/Andorra",
                                                       "Europe/Astrakhan",
                                                       "Europe/Athens",
                                                       "Europe/Belgrade",
                                                       "Europe/Berlin",
                                                       "Europe/Bratislava",
                                                       "Europe/Brussels",
                                                       "Europe/Bucharest",
                                                       "Europe/Budapest",
                                                       "Europe/Busingen",
                                                       "Europe/Chisinau",
                                                       "Europe/Copenhagen",
                                                       "Europe/Dublin",
                                                       "Europe/Gibraltar",
                                                       "Europe/Guernsey",
                                                       "Europe/Helsinki",
                                                       "Europe/Isle_of_Man",
                                                       "Europe/Istanbul",
                                                       "Europe/Jersey",
                                                       "Europe/Kaliningrad",
                                                       "Europe/Kirov",
                                                       "Europe/Kyiv",
                                                       "Europe/Lisbon",
                                                       "Europe/Ljubljana",
                                                       "Europe/London",
                                                       "Europe/Luxembourg",
                                                       "Europe/Madrid",
                                                       "Europe/Malta",
                                                       "Europe/Mariehamn",
                                                       "Europe/Minsk",
                                                       "Europe/Monaco",
                                                       "Europe/Moscow",
                                                       "Europe/Oslo",
                                                       "Europe/Paris",
                                                       "Europe/Podgorica",
                                                       "Europe/Prague",
                                                       "Europe/Riga",
                                                       "Europe/Rome",
                                                       "Europe/Samara",
                                                       "Europe/San_Marino",
                                                       "Europe/Sarajevo",
                                                       "Europe/Saratov",
                                                       "Europe/Simferopol",
                                                       "Europe/Skopje",
                                                       "Europe/Sofia",
                                                       "Europe/Stockholm",
                                                       "Europe/Tallinn",
                                                       "Europe/Tirane",
                                                       "Europe/Ulyanovsk",
                                                       "Europe/Vaduz",
                                                       "Europe/Vatican",
                                                       "Europe/Vienna",
                                                       "Europe/Vilnius",
                                                       "Europe/Volgograd",
                                                       "Europe/Warsaw",
                                                       "Europe/Zagreb",
                                                       "Europe/Zurich",
                                                       "Indian/Antananarivo",
                                                       "Indian/Chagos",
                                                       "Indian/Christmas",
                                                       "Indian/Cocos",
                                                       "Indian/Comoro",
                                                       "Indian/Kerguelen",
                                                       "Indian/Mahe",
                                                       "Indian/Maldives",
                                                       "Indian/Mauritius",
                                                       "Indian/Mayotte",
                                                       "Indian/Reunion",
                                                       "Pacific/Apia",
                                                       "Pacific/Auckland",
                                                       "Pacific/Bougainville",
                                                       "Pacific/Chatham",
                                                       "Pacific/Chuuk",
                                                       "Pacific/Easter",
                                                       "Pacific/Efate",
                                                       "Pacific/Fakaofo",
                                                       "Pacific/Fiji",
                                                       "Pacific/Funafuti",
                                                       "Pacific/Galapagos",
                                                       "Pacific/Gambier",
                                                       "Pacific/Guadalcanal",
                                                       "Pacific/Guam",
                                                       "Pacific/Honolulu",
                                                       "Pacific/Kanton",
                                                       "Pacific/Kiritimati",
                                                       "Pacific/Kosrae",
                                                       "Pacific/Kwajalein",
                                                       "Pacific/Majuro",
                                                       "Pacific/Marquesas",
                                                       "Pacific/Midway",
                                                       "Pacific/Nauru",
                                                       "Pacific/Niue",
                                                       "Pacific/Norfolk",
                                                       "Pacific/Noumea",
                                                       "Pacific/Pago_Pago",
                                                       "Pacific/Palau",
                                                       "Pacific/Pitcairn",
                                                       "Pacific/Pohnpei",
                                                       "Pacific/Port_Moresby",
                                                       "Pacific/Rarotonga",
                                                       "Pacific/Saipan",
                                                       "Pacific/Tahiti",
                                                       "Pacific/Tarawa",
                                                       "Pacific/Tongatapu",
                                                       "Pacific/Wake",
                                                       "Pacific/Wallis"};
// A fixed offset east of UTC as a POSIX TZ value ("<+0545>-5:45"; POSIX counts offsets west of UTC).
inline std::string fixed_timezone(long seconds) {
    const long magnitude = seconds < 0 ? -seconds : seconds;
    char name[16], rule[24];
    std::snprintf(name, sizeof(name), "%c%02ld%02ld", seconds < 0 ? '-' : '+', magnitude / 3600, magnitude / 60 % 60);
    if (magnitude / 60 % 60) std::snprintf(rule, sizeof(rule), "%ld:%02ld", magnitude / 3600, magnitude / 60 % 60);
    else std::snprintf(rule, sizeof(rule), "%ld", magnitude / 3600);
    return std::string("<") + name + ">" + (seconds < 0 ? "+" : "-") + rule;
}
// "America/Argentina/Buenos_Aires" -> region "America", place "Buenos Aires, Argentina".
inline std::string timezone_region(const std::string& zone) {
    auto slash = zone.find('/');
    return slash == std::string::npos ? zone : zone.substr(0, slash);
}
inline std::string timezone_place(const std::string& zone) {
    auto slash = zone.find('/');
    if (slash == std::string::npos) return zone;
    auto rest = zone.substr(slash + 1);
    auto inner = rest.find('/');
    auto place = inner == std::string::npos ? rest : rest.substr(inner + 1) + ", " + rest.substr(0, inner);
    std::replace(place.begin(), place.end(), '_', ' ');
    return place;
}
inline std::string timezone_region_name(const std::string& region) {
    return region == "America"    ? "Americas"
           : region == "Atlantic" ? "Atlantic Ocean"
           : region == "Indian"   ? "Indian Ocean"
                                  : region;
}
// The first zone of the next (+1) or previous (-1) region, for Left/Right.
inline int timezone_region_jump(int index, int direction) {
    const int count = int(timezones.size());
    index = std::clamp(index, 0, count - 1);
    auto region = [&](int i) { return timezone_region(timezones[size_t(i)]); };
    if (direction > 0) {
        auto current = region(index);
        while (index < count - 1 && region(index) == current) index++;
        return index;
    }
    auto current = region(index);
    while (index > 0 && region(index - 1) == current) index--;
    if (index == 0) return 0;
    current = region(index - 1);
    index--;
    while (index > 0 && region(index - 1) == current) index--;
    return index;
}
inline const std::array<const char*, 4> power_names = {"Stock", "Battery saver", "Balanced", "Performance"};
inline const std::array<int, 5> idle_minutes = {0, 1, 2, 5, 10};
inline std::string idle_name(int index) {
    return index == 0 ? "Never" : std::to_string(idle_minutes.at(index)) + " min";
}
// The root list's order. Section ids stay stable (saved state and tests use
// them); Display (10) is listed right below Theme and Updates (9) right below System.
inline constexpr std::array<int, 11> section_order = {0, 10, 1, 2, 3, 4, 5, 9, 6, 7, 8};
// Theme and Display share the appearance code: the row ids are the original combined list's.
inline int appearance_row_id(Page page, int index) {
    static constexpr std::array<int, 6> theme = {0, 1, 2, 3, 4, 9};
    static constexpr std::array<int, 5> display = {5, 6, 7, 8, 10};
    return page == Page::DisplayOptions ? display.at(size_t(index)) : theme.at(size_t(index));
}
inline int section_row(int section) {
    for (size_t i = 0; i < section_order.size(); i++)
        if (section_order[i] == section) return int(i);
    return 0;
}
struct Settings {
    Page page = Page::Library;
    int section = 0, focus = 0;
    // The row you left each page from. Back lands on it again instead of the
    // first row, so Library -> Artwork sources -> Back keeps Artwork sources lit.
    std::array<int, 64> remembered{};
    void remember(Page from, int row) {
        if (size_t(from) < remembered.size()) remembered[size_t(from)] = std::max(0, row);
    }
    int recall(Page target) const { return size_t(target) < remembered.size() ? remembered[size_t(target)] : 0; }
    void open() {
        page = Page::Settings;
        focus = 0;
    }
    void back() {
        if (page == Page::MenuSounds || page == Page::Haptics) {
            focus = page == Page::Haptics ? 4 : 3;
            page = Page::Sound;
        } else if (page == Page::Motion) {
            page = Page::DisplayOptions;
            focus = 1;
        } else if (page == Page::Colours) {
            page = Page::Appearance;
            focus = 1;
        } else if (page == Page::Statistics) {
            page = Page::Settings;
            section = 7;
            focus = 0;
        } else if (page == Page::Timezone) {
            page = Page::System;
            focus = 1;
        } else if (page == Page::Maintenance) {
            page = Page::System;
            focus = 5;
        } else if (page == Page::DeviceInfo) {
            page = Page::System;
            focus = 4;
        } else if (page == Page::Display) {
            page = Page::DisplayOptions;
            focus = 0;
        } else if (page == Page::Startup) {
            page = Page::System;
            focus = 8; // clamped to the last row where Startup sits earlier (RG DS OS)
        } else {
            page = page == Page::Settings || page == Page::GameOptions ? Page::Library
                   : page == Page::Scraper                             ? Page::LibraryOptions
                   : page == Page::Timezone || page == Page::DeviceInfo || page == Page::Maintenance ? Page::System
                   : page == Page::Wifi                                                              ? Page::Network
                                                                                                     : Page::Settings;
            focus = page == Page::Settings ? 0 : recall(page);
        }
    }
    void move(int delta) {
        if (page == Page::Settings) section = section_order[size_t(std::clamp(section_row(section) + delta, 0, 10))];
        else if (page == Page::Appearance) focus = std::clamp(focus + delta, 0, 5);
        else if (page == Page::DisplayOptions) focus = std::clamp(focus + delta, 0, 4);
        else if (page == Page::LibraryOptions) focus = std::clamp(focus + delta, 0, 10);
        else if (page == Page::Power) focus = std::clamp(focus + delta, 0, 6);
        else if (page == Page::Scraper) focus = std::clamp(focus + delta, 0, 4);
        else if (page == Page::Startup) focus = std::clamp(focus + delta, 0, 0);
        else if (page == Page::Timezone) focus = std::clamp(focus + delta, 0, int(timezones.size()) - 1);
        else if (page == Page::DeviceInfo) focus = std::clamp(focus + delta, 0, 4);
        else if (page == Page::System || page == Page::Display) focus = 0;
    }
    void enter() {
        if (page == Page::Settings) {
            page = section == 0    ? Page::Appearance
                   : section == 10 ? Page::DisplayOptions
                   : section == 1  ? Page::Power
                   : section == 2 ? Page::LibraryOptions
                   : section == 3 ? Page::Sound
                   : section == 5 ? Page::System
                   : section == 6 ? Page::Controls
                   : section == 7 ? Page::Statistics
                   : section == 9 ? Page::Maintenance
                                  : Page::Network;
            focus = 0;
        } else if (page == Page::DisplayOptions && focus == 0) {
            page = Page::Display;
            focus = 0;
        }
    }
};
inline SystemPreferences load_system_preferences(const fs::path& path) {
    SystemPreferences p;
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read system preferences");
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto separator = line.find('=');
        if (separator == std::string::npos) throw std::runtime_error("Invalid system preferences");
        if (!values.emplace(line.substr(0, separator), line.substr(separator + 1)).second)
            throw std::runtime_error("Duplicate system preference");
    }
    auto number = [&](const std::string& key, int low, int high) {
        auto value = values.at(key);
        size_t end = 0;
        int n = std::stoi(value, &end);
        if (end != value.size() || n < low || n > high) throw std::runtime_error("Invalid system preference: " + key);
        return n;
    };
    if (values.size() < 5 || values.size() > 10 || values.at("schema") != "1")
        throw std::runtime_error("Invalid system preferences schema");
    for (auto& pair : values)
        if (pair.first != "schema" && pair.first != "power" && pair.first != "brightness" && pair.first != "idle" &&
            pair.first != "lid" && pair.first != "battery_percent" && pair.first != "timezone" &&
            pair.first != "date_format" && pair.first != "time_format" && pair.first != "dst")
            throw std::runtime_error("Unknown system preference");
    p.power = number("power", 0, 3);
    p.brightness = number("brightness", 5, 100);
    p.idle = number("idle", 0, 4);
    p.lid = number("lid", 0, 1);
    if (values.count("battery_percent")) p.battery_percent = number("battery_percent", 0, 1);
    if (values.count("date_format")) p.date_format = number("date_format", 0, int(date_format_names.size()) - 1);
    if (values.count("time_format")) p.time_format = number("time_format", 0, 1);
    if (values.count("dst")) p.dst = number("dst", 0, int(dst_names.size()) - 1);
    if (values.count("timezone")) {
        p.timezone = values.at("timezone");
        if (std::find(timezones.begin(), timezones.end(), p.timezone) == timezones.end())
            throw std::runtime_error("Invalid timezone");
    }
    return p;
}
inline void save_system_preferences(const fs::path& path, SystemPreferences p) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    auto pending = path.string() + ".tmp";
    std::ofstream file(pending);
    file << "schema=1\npower=" << p.power << "\nbrightness=" << p.brightness << "\nidle=" << p.idle << "\nlid=" << p.lid
         << '\n';
    file << "battery_percent=" << p.battery_percent << "\ntimezone=" << p.timezone << '\n';
    if (p.date_format) file << "date_format=" << p.date_format << '\n';
    if (p.time_format) file << "time_format=" << p.time_format << '\n';
    if (p.dst) file << "dst=" << p.dst << '\n';
    file.close();
    if (!file) throw std::runtime_error("Cannot save system preferences");
    fs::rename(pending, path);
}
// The additive UI choices (ui.ini) next to the base file. Kept apart so an
// older release can still read the base file, and so a corrupt base file does
// not discard them either.
inline void load_ui_preferences(const fs::path& ui, Preferences& result) {
    std::string line;
    std::ifstream extra(ui);
    while (std::getline(extra, line)) {
        if (line == "animations=0" || line == "animations=1") result.animations = line.back() == '1';
        else if (line == "view_animations=0" || line == "view_animations=1")
            result.view_animations = line.back() == '1';
        else if (line == "view_transition_style=spring" || line == "view_transition_style=push" ||
                 line == "view_transition_style=zoom")
            result.view_style = line == "view_transition_style=push"   ? ViewStyle::Push
                                : line == "view_transition_style=zoom" ? ViewStyle::Zoom
                                                                       : ViewStyle::Spring;
        else if (line == "view_transition_speed=snappy" || line == "view_transition_speed=normal" ||
                 line == "view_transition_speed=relaxed")
            result.view_speed = line == "view_transition_speed=snappy"    ? 0
                                : line == "view_transition_speed=relaxed" ? 2
                                                                          : 1;
        else if (line == "menu_sounds=0" || line == "menu_sounds=1") result.menu_sounds = line.back() == '1';
        else if (line.rfind("sound_volume=", 0) == 0) {
            try {
                size_t end = 0;
                int value = std::stoi(line.substr(13), &end);
                if (end == line.size() - 13 && value >= 0 && value <= 100) result.sound_volume = value;
            } catch (const std::exception&) {}
        } else if (line == "touch_indicator=0" || line == "touch_indicator=1")
            result.touch_indicator = line.back() == '1';
        else if (filter_field(line, result.filter)) {
        } else if (line == "view=0" || line == "view=1" || line == "view=2") result.view = line.back() - '0';
        else if (line == "drastic_backend=0" || line == "drastic_backend=1") result.drastic_backend = line.back() - '0';
        else if (line == "icon_style=pixel" || line == "icon_style=rich") result.rich_icons = line == "icon_style=rich";
        else if (line == "high_contrast=0" || line == "high_contrast=1") result.high_contrast = line.back() == '1';
        else if (line.rfind("accent=", 0) == 0) {
            try {
                size_t end = 0;
                int value = std::stoi(line.substr(7), &end);
                if (end == line.size() - 7 && value >= -1 && value < 16) result.accent = value;
            } catch (const std::exception&) {}
        } else if (line.rfind("language=", 0) == 0) result.language = line.substr(9);
        else if (line == "layout=0" || line == "layout=1" || line == "layout=2") result.layout = line.back() - '0';
        else if (line == "home_layout=carousel" || line == "home_layout=vertical")
            result.home_layout = line == "home_layout=vertical";
        else if (line == "background=plain" || line == "background=grid") result.background = line == "background=grid";
        else if (line == "gestures=0" || line == "gestures=1") result.gestures = line.back() == '1';
        else if (line == "show_systems=0" || line == "show_systems=1") result.show_systems = line.back() == '1';
        else if (line == "menu_music=0" || line == "menu_music=1") result.menu_music = line.back() == '1';
        else if (line.rfind("music_volume=", 0) == 0) {
            try {
                size_t end = 0;
                int value = std::stoi(line.substr(13), &end);
                if (end == line.size() - 13 && value >= 0 && value <= 100) result.music_volume = value;
            } catch (const std::exception&) {}
        } else if (line == "menu_haptics=0" || line == "menu_haptics=1") result.menu_haptics = line.back() == '1';
        else if (line == "game_rumble=0" || line == "game_rumble=1") result.game_rumble = line.back() == '1';
        else if (line.rfind("haptic_strength=", 0) == 0 || line.rfind("rumble_strength=", 0) == 0) {
            try {
                const size_t start = line.find('=') + 1;
                size_t end = 0;
                int value = std::stoi(line.substr(start), &end);
                if (end == line.size() - start && value >= 10 && value <= 100)
                    (line[0] == 'h' ? result.haptic_strength : result.rumble_strength) = value;
            } catch (const std::exception&) {}
        } else if (line == "game_info=0" || line == "game_info=1") result.game_info = line.back() == '1';
        else if (line == "extra_pictures=0" || line == "extra_pictures=1" || line == "extra_pictures=2" ||
                 line == "extra_pictures=3")
            result.extra_pictures = line.back() - '0';
        else if (line == "artwork_provider=0" || line == "artwork_provider=1" || line == "artwork_provider=2" ||
                 line == "artwork_provider=3")
            result.artwork_provider = line.back() == '3' ? 0 : line.back() - '0';
    }
}
inline Preferences load_preferences(const fs::path& path) {
    Preferences result;
    std::string line;
    // The additive ui.ini is read even when the base file is absent: a fresh
    // card may carry only the UI choices (tests and development previews too).
    if (fs::exists(path)) {
        std::ifstream file(path);
        if (!file) throw std::runtime_error("Cannot read preferences");
        bool schema = false, mode = false, font = false;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "schema=1") schema = true;
            else if (line == "mode=light" || line == "mode=dark") {
                mode = true;
                result.dark = line == "mode=dark";
            } else if (line == "font=rounded" || line == "font=pixel") {
                font = true;
                result.pixel = line == "font=pixel";
            } else if (line.rfind("core=", 0) == 0) {
                unsigned value = 0;
                auto parsed = std::from_chars(line.data() + 5, line.data() + line.size(), value);
                if (parsed.ec == std::errc{} && parsed.ptr == line.data() + line.size() &&
                    value < unsigned(ds_core_count))
                    result.core = unsigned(offered_core(int(value)));
                // A newer release may know another engine. Keep the default engine
                // in that case, without discarding unrelated appearance/UI choices.
            } else if (line == "auto_load=0" || line == "auto_load=1") result.auto_load = line.back() == '1';
            else if (line == "auto_save=0" || line == "auto_save=1") result.auto_save = line.back() == '1';
            else if (line == "animations=0" || line == "animations=1") result.animations = line.back() == '1';
            else if (line == "view_animations=0" || line == "view_animations=1")
                result.view_animations = line.back() == '1';
            else if (line == "view_transition_style=spring" || line == "view_transition_style=push" ||
                     line == "view_transition_style=zoom")
                result.view_style = line == "view_transition_style=push"   ? ViewStyle::Push
                                    : line == "view_transition_style=zoom" ? ViewStyle::Zoom
                                                                           : ViewStyle::Spring;
            else if (line == "view_transition_speed=snappy" || line == "view_transition_speed=normal" ||
                     line == "view_transition_speed=relaxed")
                result.view_speed = line == "view_transition_speed=snappy"    ? 0
                                    : line == "view_transition_speed=relaxed" ? 2
                                                                              : 1;
            else if (line == "menu_sounds=0" || line == "menu_sounds=1") result.menu_sounds = line.back() == '1';
            else if (line.rfind("sound_volume=", 0) == 0) {
                try {
                    size_t end = 0;
                    int value = std::stoi(line.substr(13), &end);
                    if (end == line.size() - 13 && value >= 0 && value <= 100) result.sound_volume = value;
                } catch (const std::exception&) {}
            } else if (line == "touch_indicator=0" || line == "touch_indicator=1")
                result.touch_indicator = line.back() == '1';
            else if (filter_field(line, result.filter)) {
            } else if (line == "view=0" || line == "view=1" || line == "view=2") result.view = line.back() - '0';
            else if (!line.empty() && line[0] != '#') throw std::runtime_error("Invalid appearance preferences");
        }
        if (!schema || !mode || !font) throw std::runtime_error("Incomplete appearance preferences");
    }
    load_ui_preferences(path.parent_path() / "ui.ini", result);
    return result;
}
inline void save_preferences(const fs::path& path, Preferences p) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    auto pending = path.string() + ".tmp";
    std::ofstream file(pending);
    file << "schema=1\nmode=" << (p.dark ? "dark" : "light") << "\nfont=" << (p.pixel ? "pixel" : "rounded") << '\n';
    file << "core=" << p.core << '\n';
    file << "auto_load=" << p.auto_load << "\nauto_save=" << p.auto_save << '\n';
    file.close();
    if (!file) throw std::runtime_error("Cannot save appearance preferences");
    fs::rename(pending, path);
    auto ui = path.parent_path() / "ui.ini", draft = fs::path(ui.string() + ".pending");
    std::ofstream extra(draft);
    extra << "schema=1\nmenu_sounds=" << p.menu_sounds << "\nsound_volume=" << p.sound_volume
          << "\nanimations=" << p.animations << "\nview_animations=" << p.view_animations
          << "\ntouch_indicator=" << p.touch_indicator << "\nfilter=" << p.filter << "\nview=" << p.view
          << "\nlayout=" << p.layout << "\nartwork_provider=" << p.artwork_provider << "\ngame_info=" << p.game_info
          << "\nextra_pictures=" << p.extra_pictures << "\ndrastic_backend=" << p.drastic_backend << '\n';
    extra << "icon_style=" << (p.rich_icons ? "rich" : "pixel") << '\n';
    extra << "accent=" << p.accent << '\n';
    extra << "high_contrast=" << p.high_contrast << '\n';
    extra << "language=" << p.language << '\n';
    extra << "view_transition_style=" << view_style_name(p.view_style) << '\n';
    extra << "view_transition_speed="
          << (p.view_speed == 0   ? "snappy"
              : p.view_speed == 2 ? "relaxed"
                                  : "normal")
          << '\n';
    extra << "home_layout=" << (p.home_layout == 1 ? "vertical" : "carousel")
          << "\nbackground=" << (p.background == 1 ? "grid" : "plain") << "\ngestures=" << p.gestures << '\n';
    extra << "menu_music=" << p.menu_music << "\nmusic_volume=" << p.music_volume << '\n';
    extra << "menu_haptics=" << p.menu_haptics << "\nhaptic_strength=" << p.haptic_strength
          << "\ngame_rumble=" << p.game_rumble << "\nrumble_strength=" << p.rumble_strength << '\n';
    extra << "show_systems=" << p.show_systems << '\n';
    extra.close();
    if (!extra) throw std::runtime_error("Cannot save UI preferences");
    fs::rename(draft, ui);
}
inline bool dark_theme_file(const fs::path& path) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "mode=dark") return true;
        if (line == "mode=light") return false;
    }
    return path.stem() == "dark";
}
}
