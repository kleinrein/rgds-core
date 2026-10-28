#pragma once
// Input: buttons and keys (action), touch and mouse pointers, raw touch
// points and the SDL event loop that routes them (event, handle_input).
// Members of App in main.cpp; included once, right after the class.

void App::action(int code) {
    if (shutting_down()) return;
    SoundAction sound_action(*this, code);
    if (code == 0 || code == 1 || code == 8 || code == 9 || code == 3) sound_code = code;
    motion_input(code);
    if (launching) {
        if (code == 3) cancel_launch();
        return;
    }
    if (chat_open) {
        chat_action(code);
        return;
    }
    brightness_drag = false;
    volume_drag = false;
    last_activity = s.GetTicks();
    if (picker_open && picker_title == "Jump to letter" && (code == 15 || code == 16)) {
        picker_action(3);
        if (code == 16) quick_search();
        return;
    }
    if (picker_open) {
        picker_action(code);
        return;
    }
    if (filter_sheet_active()) {
        filter_sheet_key(code);
        return;
    }
    if (settings.page == Page::Keyboard && edit_field == 4 && (code == 15 || code == 16)) {
        finish_edit(false);
        if (code == 15) jump_to_letter();
        return;
    }
    if (settings.page == Page::GameSwitcher && home_switcher) {
        // From the launcher: A plays the game, B goes back to where the button was pressed.
        if (code == 3) close_home_switcher();
        else if (code == 8 || code == 9 || code == 0 || code == 1)
            settings.focus = std::clamp(settings.focus + (code == 8 || code == 0 ? -1 : 1), 0, list_count() - 1);
        else if ((code == 2 || code == 4) && settings.focus < int(switch_games.size()))
            play_game(switch_games[size_t(settings.focus)]);
        dirty = true;
        return;
    }
    if (settings.page == Page::GameSwitcher) {
        if (menu_status["state"] == "busy" || !menu_pending_token.empty()) return;
        if (code == 7) menu_request("resume");
        else if (code == 3) {
            settings.page = Page::QuickMenu;
            settings.focus = 0;
        } else if (code == 8 || code == 9 || code == 0 || code == 1)
            settings.focus = std::clamp(settings.focus + (code == 8 || code == 0 ? -1 : 1), 0, list_count() - 1);
        else if (code == 11 || ((code == 2 || code == 4) && settings.focus == int(switch_games.size()))) {
            settings.page = Page::QuickMenu;
            settings.focus = 0;
        } else if (code == 2 || code == 4)
            menu_request(settings.focus == 0 ? "resume" : "switch:" + std::to_string(settings.focus));
        dirty = true;
        return;
    }
    if ((code == 0 || code == 1 || code == 2 || code == 4) && open_setting_picker()) return;
    if (settings.page == Page::ShaderPicker) {
        if (code == 3) close_shader_picker();
        else if (code == 0 || code == 1) focus_shader(settings.focus + (code == 0 ? -1 : 1));
        else if (code == 8 || code == 9 || code == 13 || code == 14)
            shader_family_step(code == 8 || code == 13 ? -1 : 1);
        else if (code == 2 || code == 4) choose_shader();
        else if (code == 11 || code == 12) change_shader_preview(code);
        dirty = true;
        return;
    }
    if (settings.page == Page::MenuSounds) {
        menu_sounds_action(code);
        return;
    }
    if (settings.page == Page::Haptics) {
        haptics_action(code);
        return;
    }
    if (settings.page == Page::Colours) {
        colours_action(code);
        return;
    }
    if (settings.page == Page::Motion) {
        if (code == 3) settings.back();
        else if (code == 8 || code == 9) settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 3);
        else if (code == 0 || code == 1 || code == 2) {
            // Left/right step through the three choices. A is not a stepper: a
            // pill that slid on A read as a confirm, so it leaves the choice alone.
            if (code == 2 && settings.focus >= 2) return;
            auto step = [&](int value) { return code == 0 ? std::max(0, value - 1) : std::min(2, value + 1); };
            if (settings.focus == 2) {
                const int style = step(preferences.view_style == ViewStyle::Push   ? 1
                                       : preferences.view_style == ViewStyle::Zoom ? 2
                                                                                   : 0);
                preferences.view_style = style == 1   ? ViewStyle::Push
                                         : style == 2 ? ViewStyle::Zoom
                                                      : ViewStyle::Spring;
                remember_appearance();
                dirty = true;
                return;
            }
            if (settings.focus == 3) {
                preferences.view_speed = step(preferences.view_speed);
                remember_appearance();
                dirty = true;
                return;
            }
            bool& value = settings.focus == 0 ? preferences.animations : preferences.view_animations;
            value = code == 2 ? !value : code == 1;
            if (settings.focus == 1 && value) preferences.animations = true;
            if (settings.focus == 0 && !value) preferences.view_animations = false;
            remember_appearance();
            if (preferences.view_animations) start_view_motion(s.GetTicks());
        }
        dirty = true;
        return;
    }
    if (settings.page == Page::Microphone) {
        if (code == 3) {
            stop_microphone();
            settings.page = Page::Sound;
            settings.focus = 2;
            configure_sounds();
            dirty = true;
        }
        return;
    }
    if (settings.page == Page::Controls) {
        controls_action(code);
        return;
    }
    if (settings.page == Page::PortFiles) {
        port_files_action(code);
        return;
    }
    if (settings.page == Page::SaveGallery || settings.page == Page::RomSetup || settings.page == Page::DSControls) {
        auto page = settings.page;
        if (code == 3) {
            back_to(page == Page::SaveGallery  ? gallery_return
                    : page == Page::DSControls ? ds_return
                                               : Page::LibraryOptions);
        } else if (code == 8 || code == 9)
            settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, list_count() - 1);
        else if (code == 2 || code == 4) {
            if (page == Page::SaveGallery) slot_primary();
            else if (page == Page::RomSetup) {
                if (settings.focus == 0)
                    show_detail(
                        "Add your games",
                        locale.text(
                            "Copy your .nds files or DS ZIP archives to Roms/NDS on the games card. Games are not included. Saves stay in userdata."));
                else if (settings.focus == 1) {
                    if (library_ready) {
                        setup_scanned = true;
                        rescan_games(true);
                    }
                } else {
                    fs::create_directories(o.data);
                    std::ofstream(o.data / "setup-complete").put('1');
                    setup_scanned = false;
                    if (setup_first_run) {
                        setup_first_run = false;
                        settings.page = Page::Library;
                        home_menu = true;
                    } else back_to(Page::LibraryOptions);
                }
            } else ds_controls_action();
        } else if (page == Page::SaveGallery && code == 11) slot_delete();
        else if (page == Page::SaveGallery && code == 12) slot_more();
        dirty = true;
        return;
    }
    if (o.game_menu) {
        if (settings.page == Page::Display && code == 2) {
            settings.page = Page::QuickMenu;
            settings.focus = 6;
            dirty = true;
            return;
        }
        if (settings.page == Page::ApplyShader) {
            if (code == 3 || code == 7) {
                settings.page = Page::QuickMenu;
                settings.focus = 3;
            } else if (code == 8 || code == 9) settings.focus = code == 8 ? 0 : 1;
            else if (code == 2 || code == 4) {
                if (settings.focus == 0) menu_request("restart");
                else {
                    settings.page = Page::QuickMenu;
                    settings.focus = 3;
                }
            }
            dirty = true;
            return;
        }
        if (settings.page == Page::QuickMenu && port_manager()) {
            port_manager_action(code);
            return;
        }
        if (settings.page == Page::QuickMenu) {
            if (code == 3 || code == 7) menu_request("resume");
            else if (code == 11) open_emulator_options();
            else if (code == 8 || code == 9) {
                const std::array<int, 12> up = {0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 9},
                                          down = {1, 4, 5, 6, 7, 8, 9, 10, 11, 11, 10, 11};
                settings.focus = (code == 8 ? up : down).at(settings.focus);
            } else if (code == 0 || code == 1) {
                if (settings.focus >= 10) settings.focus = code == 0 ? 10 : 11;
                else if (settings.focus != 0) {
                    int column = (settings.focus - 1) % 3;
                    if ((code == 0 && column > 0) || (code == 1 && column < 2)) settings.focus += code == 0 ? -1 : 1;
                }
            } else if (code == 2 || code == 4) quick_action();
            dirty = true;
            return;
        }
        if (settings.page == Page::LeaveGame) {
            if (code == 3) {
                settings.page = Page::QuickMenu;
                settings.focus = 0;
            } else if (code == 8 || code == 9) settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 2);
            else if (code == 2) {
                if (settings.focus == 0) {
                    settings.page = Page::QuickMenu;
                    menu_request("quit");
                } else menu_request(settings.focus == 1 ? "resume" : "force-quit");
            }
            dirty = true;
            return;
        }
        if (settings.page == Page::Cheats) {
            if (code == 7) {
                settings.page = Page::QuickMenu;
                settings.focus = 11;
            } else if (code == 3) cheat_back();
            else if (code == 8 || code == 9) cheat_move(code == 8 ? -1 : 1);
            else if (code == 13 || code == 14) cheat_tab_step(code == 13 ? -1 : 1);
            else if (code == 11) begin_edit(6);
            else if (code == 0 || code == 1 || code == 2 || code == 4) cheat_action(code);
            dirty = true;
            return;
        }
        if (settings.page == Page::PlayGraphics) {
            if (code == 3 || code == 7) {
                settings.page = Page::QuickMenu;
                settings.focus = 3;
            } else if (code == 8 || code == 9) {
                settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, list_count() - 1);
                status.clear();
            } else if (code == 2 || code == 4 || code == 0 || code == 1) play_graphics_action();
            dirty = true;
            return;
        }
        if (settings.page == Page::Graphics) {
            if (code == 3 || code == 7) {
                settings.page = Page::QuickMenu;
                settings.focus = 3;
            } else if (code == 8 || code == 9) {
                settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, list_count() - 1);
                status.clear();
            } else if (code == 2 || code == 4 || code == 0 || code == 1) graphics_action();
            dirty = true;
            return;
        }
        if (settings.page == Page::EmulatorOptions) {
            if (code == 3 || code == 7) {
                settings.page = Page::QuickMenu;
                settings.focus = 4;
            } else if (code == 8 || code == 9)
                settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, list_count() - 1);
            else if (code == 0 || code == 1 || code == 2) change_emulator_option(code == 0 ? -1 : 1);
            dirty = true;
            return;
        }
        if (code == 3 || code == 7) {
            settings.page = Page::QuickMenu;
            settings.focus = 0;
            dirty = true;
            return;
        }
    }
    if (settings.page == Page::GameInfo) {
        game_info_key(code);
        return;
    }
    if (settings.page == Page::GameArtwork) {
        if (code == 3) {
            back_to(artwork_return);
            if (artwork_return == Page::GameOptions) settings.focus = 4;
        } else if (code == 8 || code == 9) settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, 2);
        else if (code == 2) game_artwork_action();
        dirty = true;
        return;
    }
    if (settings.page == Page::Keyboard) {
        if (code == 3 || code == 7) finish_edit(false);
        else if (code == 4) finish_edit(true);
        else if (code == 2) keyboard_choose();
        else if (code == 11) erase_character();
        else if (code == 12) key_layer = key_layer == 0 ? 1 : 0;
        else keyboard_move(code);
        dirty = true;
        return;
    }
    if (settings.page == Page::Confirm) {
        if (code == 3) {
            back_to(confirm_return);
        } else if (code == 0 || code == 1) settings.focus = code == 0 ? 0 : 1;
        else if (code == 2 || code == 4) {
            bool accepted = settings.focus == 1;
            settings.page = confirm_return;
            settings.focus = 0;
            if (accepted) {
                if (confirmation == "return-stock") quit = true;
                else if (confirmation == "startup-rgds") save_startup();
                else if (confirmation == "bt-forget") bluetooth_forget();
                else if (confirmation == "remove-game") remove_game();
                else {
                    start_worker("device." + confirmation);
                    if (confirmation == "poweroff" || confirmation == "reboot") begin_shutdown(confirmation);
                }
            }
        }
        dirty = true;
        return;
    }
    if ((code == 8 || code == 9) &&
        (settings.page == Page::Network || settings.page == Page::Wifi || settings.page == Page::Bluetooth ||
         settings.page == Page::Homebrew || settings.page == Page::System || settings.page == Page::Maintenance ||
         settings.page == Page::GameOptions)) {
        int maximum =
            settings.page == Page::System ? list_count() - 1
            : (settings.page == Page::GameOptions || settings.page == Page::Maintenance ||
               settings.page == Page::Network || settings.page == Page::Bluetooth || settings.page == Page::Homebrew)
                ? list_count() - 1
                : std::max(0, std::atoi(device_status["network_count"].c_str()) - 1);
        settings.focus = std::clamp(settings.focus + (code == 8 ? -1 : 1), 0, maximum);
        dirty = true;
        return;
    }
    if (code == 7 && !probe) {
        if (settings.page == Page::Library) settings.open();
        else settings.page = Page::Library;
        dirty = true;
        return;
    }
    if (!probe && settings.page != Page::Library && code != 5 && code != 6) {
        if (code == 3 && settings.page == Page::Maintenance && updates_return != Page::System) {
            if (updates_return == Page::Library) settings.page = Page::Library;
            else {
                settings.page = Page::Settings;
                settings.section = 9;
            }
            updates_return = Page::Settings;
            view_route = ViewTravel::Back;
        } else if (code == 3) {
            bool game = settings.page == Page::GameOptions, shelf = settings.page == Page::Homebrew;
            settings.back();
            if (settings.page == Page::System) settings.focus = std::min(settings.focus, list_count() - 1);
            if (shelf) {
                // back on the Homebrew tile; what was downloaded is in its system already
                rescan_games(false);
                refresh_other_systems();
                open_systems();
                const auto at = std::find(listed_systems.begin(), listed_systems.end(), "homebrew");
                if (at != listed_systems.end()) {
                    systems_selected = size_t(at - listed_systems.begin());
                    carousel.reset(systems_selected);
                }
            } else if (game) library_view(preferences.view);
        } else if (code == 8 || code == 9) {
            settings.move(code == 8 ? -1 : 1);
            if (settings.page == Page::Scraper) settings.focus = std::min(settings.focus, list_count() - 1);
        } else if (settings.page == Page::Appearance || settings.page == Page::DisplayOptions) {
            if (code == 0 || code == 1 || code == 2) choose_appearance(code);
        } else if (settings.page == Page::Power) {
            const int id = power_rows[size_t(std::clamp(settings.focus, 0, int(power_rows.size()) - 1))];
            if ((id < 4 || id == 7 || id == 8) && (code == 0 || code == 1 || code == 2))
                change_system(id, code == 0 ? -1 : 1, code == 2);
            else if (id == 4 && code == 2) sleep_now();
            else if (id == 5 && code == 2 && hardware.poweroff) confirm("poweroff", Page::Power);
            else if (id == 6 && code == 2 && hardware.poweroff) confirm("reboot", Page::Power);
        } else if (settings.page == Page::Display) {
            if (code == 0 || code == 1) change_system(4, code == 0 ? -1 : 1);
            else if (code == 2) {
                flush_brightness();
                settings.back();
            }
        } else if (settings.page == Page::Sound) {
            // Left and Right only move the slider while the volume row is the selected one.
            if (code == 0 || code == 1) {
                if (settings.focus == 0) change_volume(hardware.volume_percent + (code == 0 ? -5 : 5));
            } else if (code == 2) {
                if (settings.focus == 3 || settings.focus == 4) {
                    settings.page = settings.focus == 4 ? Page::Haptics : Page::MenuSounds;
                    settings.focus = 0;
                } else if (settings.focus == 2) open_microphone();
                else toggle_mute();
            }
        } else if (settings.page == Page::LibraryOptions) {
            const int row = library_row(settings.focus);
            if (row == 10) {
                if (code == 2 && library_ready) rescan_games(true);
            } else if (row == 0 && (code == 0 || code == 1 || code == 2)) {
                {
                    int core = int(preferences.core);
                    do
                        core = code == 2 ? (core + 1) % ds_core_count
                                         : std::clamp(core + (code == 0 ? -1 : 1), 0, ds_core_count - 1);
                    while (!core_offered(core));
                    preferences.core = unsigned(core);
                }
                remember_appearance();
            } else if (row == 1 && code == 2) open_scraper();
            else if (row < 4 && row >= 2 && (code == 0 || code == 1 || code == 2)) {
                bool& value = row == 2 ? preferences.auto_load : preferences.auto_save;
                value = code == 2 ? !value : code == 1;
                remember_appearance();
            } else if (row == 4 && (code == 0 || code == 1 || code == 2)) open_shader_picker(false);
            else if (row == 9 && (code == 0 || code == 1 || code == 2)) {
                preferences.show_systems = code == 2 ? !preferences.show_systems : code == 1;
                remember_appearance();
                refresh_other_systems();
                summary_ready = false;
                library_summary.request(o.data, all_games);
                library_view(0);
            } else if (row == 7 && code == 2) {
                settings.page = Page::RomSetup;
                settings.focus = 0;
            } else if (row == 8 && code == 2) {
                if (worker <= 0) {
                    backup_requested = true;
                    start_worker("device.library", "{\"action\":\"backup\"}");
                }
            } else if (row == 5 && (code == 0 || code == 1 || code == 2)) {
                preferences.layout = (preferences.layout + (code == 0 ? 2 : 1)) % 3;
                remember_appearance();
            }
        } else if (settings.page == Page::GameOptions) {
            const int base = game_options_base();
            if (settings.focus == base) {
                if (code == 2 && game_removable()) confirm("remove-game", Page::GameOptions);
            } else if (other_system_selected() && (settings.focus == 1 || settings.focus == base - 1)) {
            } else if (code == 0 || code == 1 || code == 2) {
                if (settings.focus == base - 3) open_game_info(Page::GameOptions);
                else if (settings.focus == base - 2) open_gallery();
                else if (settings.focus == base - 1) open_ds_controls();
                else if (settings.focus == 4) open_game_artwork();
                else if (settings.focus == 3) open_controls(true, Page::GameOptions);
                else if (settings.focus == 2) open_shader_picker(true);
                else change_game_option(code == 0 ? -1 : 1, code == 2);
            }
        } else if (settings.page == Page::System) {
            if (code == 2) {
                if (settings.focus == 1) {
                    settings.page = Page::Timezone;
                    auto found = std::find(timezones.begin(), timezones.end(), system_preferences.timezone);
                    settings.focus = int(found - timezones.begin());
                } else if (settings.focus == 5) {
                    settings.page = Page::DeviceInfo;
                    settings.focus = 0;
                    start_worker("device.info");
                } else if (settings.focus == 6) {
                    open_updates(Page::System);
                } else if (settings.focus == 7 && !o.rgds_os) confirm("return-stock", Page::System);
                else if (settings.focus == (o.rgds_os ? 7 : 8)) change_startup(0, true);
            }
        } else if (settings.page == Page::Network) {
            if (code == 2) {
                if (settings.focus == 0) {
                    if (device_status["wifi"] == "1") confirm("wifi-off", Page::Network);
                    else start_worker("device.wifi-on");
                } else if (settings.focus == 1) {
                    settings.page = Page::Wifi;
                    settings.focus = 0;
                    start_worker("device.scan");
                } else if (settings.focus == 2) {
                    if (device_status["bt_supported"] == "0")
                        service_notice = "Bluetooth is not available on this handheld.";
                    else if (device_status["bt"] == "1") confirm("bt-off", Page::Network);
                    else start_worker("device.bt-on");
                } else if (settings.focus == 3) {
                    if (device_status["bt_supported"] == "0")
                        service_notice = "Bluetooth is not available on this handheld.";
                    else {
                        settings.page = Page::Bluetooth;
                        settings.focus = 0;
                        start_worker("device.bt-scan");
                    }
                } else if (settings.focus == 4) start_worker("device.info");
                else if (settings.focus == 5) show_sharing_picker();
                else if (settings.focus == 6 && device_status["sharing"] == "1") {
                    auto login = fields(device_status["sharing_login"]);
                    if (device_status["sharing_mode"] == "open")
                        show_detail(
                            "Sharing details",
                            "Mac: Finder > Network > " +
                                (device_status["sharing_name"].empty() ? std::string("RG DS")
                                                                       : device_status["sharing_name"]) +
                                "\nOr connect to " + device_status["sharing_address"] + "\nWindows: \\\\" +
                                device_status["ip"] +
                                "\nNo password needed: connect as Guest.\nFolders: " + login["shares"] +
                                "\nAnyone on this Wi-Fi can open these folders. Use Password protected on shared networks.");
                    else
                        show_detail("Sharing details", device_status["sharing_address"] + "\nWindows: \\\\" +
                                                           device_status["ip"] + "\nUsername: " + login["username"] +
                                                           "\nPassword: " + login["password"] +
                                                           "\nFolders: " + login["shares"] +
                                                           "\nLogin changes each time sharing is turned on.");
                }
            }
        } else if (settings.page == Page::Wifi) {
            if (code == 2) wifi_choose();
            else if (code == 11) start_worker("device.scan");
        } else if (settings.page == Page::Homebrew) {
            if (code == 2) homebrew_choose();
            else if (code == 13 || code == 14) homebrew_step(code == 13 ? -1 : 1);
        } else if (settings.page == Page::Bluetooth) {
            if (code == 2) bluetooth_choose();
            else if (code == 11) start_worker("device.bt-scan");
            else if (code == 12 && list_count() > 0 && worker <= 0) {
                bluetooth_address = device_status["bt_" + std::to_string(settings.focus) + "_address"];
                if (device_status["bt_" + std::to_string(settings.focus) + "_paired"] == "1")
                    confirm("bt-forget", Page::Bluetooth);
            }
        } else if (settings.page == Page::Maintenance) {
            if (code == 12) update_now();
            else if (code == 2) {
                bool busy = worker > 0 && worker_action.rfind("device.update-", 0) == 0;
                if (busy) {
                    if (settings.focus == 2 &&
                        (worker_action == "device.update-download" ||
                         (worker_action == "device.update-auto" && update_status["state"] == "downloading")))
                        stop_worker();
                } else if (settings.focus == 0) start_worker("device.update-check");
                else if (settings.focus == 1) {
                    if (update_status["prepared"] == "1") confirm("update-apply", Page::Maintenance);
                    else if (update_status["state"] == "available" || update_status["state"] == "cancelled" ||
                             update_status["state"] == "error")
                        start_worker("device.update-download");
                } else if (settings.focus == 2)
                    show_detail("Release notes", update_status["notes"].empty() ? locale.text("No release notes yet.")
                                                                                : update_status["notes"]);
                else if (settings.focus == 3) {
                    if (device_status["previous"].empty()) service_notice = "No previous build available.";
                    else confirm("rollback", Page::Maintenance);
                } else if (settings.focus == 6)
                    show_picker("Automatic updates", {"Off", "Check automatically", "Download automatically"},
                                std::clamp(std::atoi(experience["auto_update"].c_str()), 0, 2),
                                [this](int n) { save_experience("auto_update", n); });
                else if (settings.focus == 4) confirm("clear-cache", Page::Maintenance);
                else if (update_status["prepared"] == "1") start_worker("device.update-discard");
                else start_worker("device.diagnostics");
            }
        } else if (settings.page == Page::Timezone) {
            if (code == 2) save_timezone();
            else if (code == 0 || code == 1) {
                settings.focus = timezone_region_jump(settings.focus, code == 0 ? -1 : 1);
            }
        } else if (settings.page == Page::Scraper) {
            if (code == 2) scraper_action();
        } else if (code == 2) {
            if (settings.page == Page::Settings && settings.section == 8) {
                if (hardware.poweroff) confirm("poweroff", Page::Settings);
            } else if (settings.page == Page::Settings && settings.section == 6) open_controls(false);
            else if (settings.page == Page::Settings && settings.section == 9) open_updates(Page::Settings);
            else if (settings.page == Page::DeviceInfo) open_device_detail();
            else {
                settings.enter();
                if (settings.page == Page::System) start_worker("device.startup-info");
            }
        }
        dirty = true;
        return;
    }
    if (!home_menu && (code == 15 || code == 16)) {
        if (code == 15) jump_to_letter();
        else quick_search();
        return;
    }
    if (home_menu && preferences.home_layout == 1 && (code == 0 || code == 1 || code == 8 || code == 9)) {
        home_column_move(code);
        return;
    }
    if (home_menu && preferences.home_layout == 1 && home_bar_focus >= 0 && (code == 2 || code == 4)) {
        home_bar_activate(home_bar_focus);
        return;
    }
    if (!home_menu && (code == 13 || code == 14)) {
        navigate(code == 13 ? -1 : 1);
        return;
    }
    if (!home_menu && browse_layout() == 2 && (code == 8 || code == 9)) {
        navigate(code == 8 ? -1 : 1);
        return;
    }
    if (code == 0) navigate(-1);
    else if (code == 1) navigate(1);
    else if (code == 2) launch();
    else if (code == 3) {
        if (probe) quit = true;
        else if (systems_menu) {
            systems_menu = false;
            return_home();
        } else if (!home_menu && preferences.show_systems && preferences.view == 0) open_systems();
        else if (!home_menu) return_home();
    } else if (code == 4) launch();
    else if (code == 5) probe = !probe;
    else if (code == 6) {
        try {
            reload_themes();
            status = "THEME RELOADED";
            std::cout << "Theme reloaded: " << o.theme << std::endl;
        } catch (const std::exception& e) {
            status = "THEME ERROR - RETAINED CURRENT";
            std::cerr << e.what() << std::endl;
        }
    } else if (code == 11 && !home_menu) open_game_options();
    else if (code == 12 && !home_menu && !systems_menu) choose_library_view();
    else if (code == 12 && home_menu) update_now();
    dirty = true;
}

void App::touch(int x, int y) {
    if (shutting_down()) return;
    motion_input(2);
    last_activity = s.GetTicks();
    brightness_drag = false;
    volume_drag = false;
    touch_x = x;
    touch_y = y;
    last_input = "TOUCH " + std::to_string(x) + ", " + std::to_string(y);
    dirty = true;
    if (probe) return;
    if (picker_open) {
        picker_touch(x, y);
        return;
    }
    if (filter_sheet_active()) {
        filter_sheet_touch(x, y);
        return;
    }
    if (settings.page == Page::GameSwitcher && !home_switcher && y >= 424 && x >= 200 && x < 440) {
        action(11);
        return;
    }
    if (settings.page != Page::Library && x >= 24 && x <= 76 && y >= 8 && y <= 72) {
        action(3);
        return;
    }
    if (settings.page == Page::ShaderPicker) {
        if (y >= 24 && y <= 66) {
            if (x >= 368 && x <= 488) change_shader_preview(11);
            else if (x >= 496 && x <= 624) change_shader_preview(12);
            return;
        }
        const auto shown = shader_families_shown();
        for (size_t place = 0; place < shown.size(); place++)
            if (point_in(shader_chip(int(place)), x, y)) {
                shader_family_jump(shown[place]);
                return;
            }
        return;
    }
    if (settings.page == Page::SaveGallery && y >= 424 && x >= 200 && x < 440) {
        action(x < 320 ? 11 : 12);
        return;
    }
    if (settings.page == Page::Maintenance && y >= 424 && x >= 200 && x < 440) {
        action(12);
        return;
    }
    if (settings.page == Page::GameInfo && y >= 78 && y < 424) {
        game_info_touch(x, y);
        return;
    }
    if (settings.page != Page::Library && y >= 424) {
        if (x < 200) action(3);
        else if (x >= 440) action(settings.page == Page::Keyboard ? 4 : 2);
        return;
    }
    if (settings.page == Page::Controls && control_capture) {
        controls_action(3);
        return;
    }
    if (list_count() > 0 || settings.page == Page::Wifi || settings.page == Page::Bluetooth ||
        settings.page == Page::Homebrew || settings.page == Page::Cheats) {
        sync_list();
        if ((settings.page == Page::Wifi || settings.page == Page::Bluetooth) && x >= 476 && y >= 24 && y <= 66) {
            start_worker(settings.page == Page::Wifi ? "device.scan" : "device.bt-scan");
            return;
        }
        if (settings.page == Page::Cheats && cheat_header_hit(x, y)) {
            cheat_header_tap(x);
            return;
        }
        if (x >= 596 && y >= list_top && y < list_top + list_height) {
            int thumb = std::max(54, list_height * list_height / std::max(1, list_count() * list_stride - 16));
            int top = list_top + (list_max_scroll() ? list_scroll * (list_height - thumb) / list_max_scroll() : 0);
            scroll_grab = y >= top && y < top + thumb ? y - top : thumb / 2;
            scroll_drag = true;
            scroll_cue_at = s.GetTicks();
            micro_until = scroll_cue_at + 90;
            scroll_to(y);
            return;
        }
        if (x >= 24 && x < 590 && y >= list_top && y < list_top + list_height) {
            int local = y - list_top + list_scroll, index = local / list_stride;
            if (local % list_stride < 66 && index < list_count()) {
                if (settings.page == Page::Settings) settings.section = section_order[size_t(index)];
                else settings.focus = index;
                if (settings.page == Page::Sound && index == 0) {
                    volume_drag = true;
                    change_volume(
                        std::clamp(int(std::round((x - sound_track_x) * 20.0 / sound_track_width)) * 5, 0, 100));
                } else if (settings.page == Page::EmulatorOptions) {
                    if (!open_setting_picker()) change_emulator_option(x < 420 ? -1 : 1);
                } else if (settings.page == Page::Appearance && (index == 0 || index == 3) &&
                           x >= segment_bounds(index, 0).x - 4)
                    choose_appearance(x >= segment_bounds(index, 1).x ? 1 : 0);
                else action(2);
            }
        }
        return;
    }
    if (settings.page == Page::Colours) {
        colours_touch(x, y);
        return;
    }
    if (settings.page == Page::Motion) {
        if (x < 28 || x > 612 || y < motion_row(0) || y >= motion_row(4) || (y - motion_row(0)) % 82 >= 72) return;
        settings.focus = (y - motion_row(0)) / 82;
        if (settings.focus < 2) {
            action(2);
            return;
        }
        // A tap on a choice selects it directly.
        for (int i = 0; i < 3; i++)
            if (x >= motion_segment_x(i) && x < motion_segment_x(i) + 104) {
                if (settings.focus == 2)
                    preferences.view_style = i == 1 ? ViewStyle::Push : i == 2 ? ViewStyle::Zoom : ViewStyle::Spring;
                else preferences.view_speed = i;
                remember_appearance();
                dirty = true;
            }
        return;
    }
    if (settings.page == Page::ApplyShader) {
        if (x >= 28 && x <= 612 && y >= 174 && y < 326) {
            int index = (y - 174) / 86;
            if ((y - 174) % 86 < 66) {
                settings.focus = index;
                action(2);
            }
        }
        return;
    }
    if (settings.page == Page::QuickMenu && port_manager()) {
        port_manager_touch(x, y);
        return;
    }
    if (settings.page == Page::QuickMenu) {
        if (x >= 28 && x <= 612) {
            if (y >= quick_top && y < quick_top + quick_wide_height) settings.focus = 0;
            else if (y >= quick_grid_top && y < quick_grid_top + 3 * quick_grid_pitch &&
                     (y - quick_grid_top) % quick_grid_pitch < quick_tile_height && (x - 28) % 200 < 184)
                settings.focus = 1 + ((y - quick_grid_top) / quick_grid_pitch) * 3 + (x - 28) / 200;
            else if (y >= quick_bottom_top && y < quick_bottom_top + quick_wide_height) {
                if (x > 312 && x < 328) return;
                settings.focus = x >= 328 ? 11 : 10;
            } else return;
            quick_action();
        }
        return;
    }
    if (settings.page == Page::LeaveGame) {
        if (x >= 28 && x <= 612 && y >= 210 && y < 408) {
            int index = (y - 210) / 70;
            if ((y - 210) % 70 < 58) {
                settings.focus = index;
                action(2);
            }
        }
        return;
    }
    if (settings.page != Page::Library) {
        if (settings.page == Page::Keyboard) {
            if (!edit_buffer.empty() && point_in({568, 103, 40, 38}, x, y)) {
                edit_buffer.clear();
                edit_cursor = 0;
                search_changed();
                return;
            }
            auto rows = keyboard_rows();
            for (size_t row = 0; row < rows.size(); row++)
                for (size_t col = 0; col < rows[row].size(); col++)
                    if (point_in(rows[row][col].bounds, x, y)) {
                        key_row = int(row);
                        key_col = int(col);
                        keyboard_choose();
                        return;
                    }
        } else if (settings.page == Page::Display && x >= 44 && x <= 596 && y >= slider_band().first &&
                   y <= slider_band().second) {
            brightness_drag = true;
            int value = std::clamp(5 + int(std::round((x - 56) * 19.0 / 528)) * 5, 5, 100);
            change_system(4, value / 5 - system_preferences.brightness / 5);
        } else if (settings.page == Page::Confirm && y >= 365 && y <= 413 && x >= 28 && x <= 612) {
            settings.focus = x >= 332 ? 1 : 0;
            action(2);
        }
        dirty = true;
        return;
    }
    if (home_menu && preferences.home_layout == 1) {
        home_column_touch(x, y);
        return;
    }
    if (!home_menu) {
        auto header = library_header_layout();
        auto inside = [&](SDL_Rect bounds) {
            return x >= bounds.x && x < bounds.x + bounds.w && y >= bounds.y && y < bounds.y + bounds.h;
        };
        if (inside(header.back)) action(3);
        else if (inside(header.view)) action(12);
        else if (inside(header.options)) action(11);
        else if (discovery_active() && inside(header.clear)) clear_discovery();
        else if (inside(header.search)) quick_search();
        else if (discovery_active() && model.games.empty() && point_in({80, 326, 232, 48}, x, y)) quick_search();
        else if (discovery_active() && model.games.empty() && point_in({328, 326, 232, 48}, x, y)) clear_discovery();
        else if (favourites_empty() && point_in({160, 326, 320, 48}, x, y)) browse_from_favourites();
    }
}

int App::cover_hit(int x, int y) {
    int play_width = std::max(92, label_width("Play", 16) + 48);
    if (y >= 386 && y < 414 && x >= 320 - play_width / 2 && x < 320 + play_width / 2) return int(model.selected);
    int found = -1;
    double nearest = 10;
    for (int index = std::max(0, int(std::floor(carousel.position)) - 3);
         index < std::min(int(navigation_count()), int(std::floor(carousel.position)) + 4); index++) {
        double distance = index - carousel.position, depth = std::min(2.0, std::abs(distance));
        int size = int(std::lround(224 - depth * 34)),
            left = int(std::lround(320 + distance * carousel.item_spacing)) - size / 2, top = 151 + int(depth * 18);
        auto bounds =
            media.fitted_box(surfaces.back().renderer, item_folder(size_t(index)) / "box.bmp", {left, top, size, size});
        if (x >= bounds.x - 3 && x < bounds.x + bounds.w + 3 && y >= bounds.y - 3 && y < bounds.y + bounds.h + 3 &&
            std::abs(distance) < nearest) {
            found = index;
            nearest = std::abs(distance);
        }
    }
    return found;
}

void App::pointer_begin(int x, int y) {
    if (chat_open) {
        view_touch_dx = view_touch_dy = 0;
        contact_x = down_x = x;
        contact_y = down_y = y;
        pointer_down = true;
        touch_tail = false;
        pointer_at = s.GetTicks();
        // As elsewhere, an edge touch waits: a swipe goes Home or Back, a release where it began is a plain tap.
        edge_gesture = gestures_available() ? (y >= 470  ? EdgeGesture::Bottom
                                               : x <= 10 ? EdgeGesture::Left
                                                         : EdgeGesture::None)
                                            : EdgeGesture::None;
        if (edge_gesture == EdgeGesture::None) chat_press(x, y);
        return;
    }
    hold.release(4000);
    if (launching) {
        pointer_source = PointerSource::None;
        return;
    }
    // Hit-test the view actually under the finger, then settle it so a
    // held gesture is never chasing moving controls. Preserve its offset.
    motion_touch = true;
    auto point = view_motion.unproject(x, y, view_drawn_at);
    // Pixels belonging to the departing page cannot activate the new page.
    if (preferences.view_style == ViewStyle::Push && view_motion.running && view_previous_ready) {
        auto layers = view_motion.push(view_drawn_at);
        if (point.first < 0 || point.first >= 640 || (!layers.incoming_above && x >= layers.outgoing)) {
            motion_touch = false;
            pointer_source = PointerSource::None;
            return;
        }
    }
    point = motion_hits.unproject(point.first, point.second);
    view_touch_dx = point.first - x;
    view_touch_dy = point.second - y;
    x = point.first;
    y = point.second;
    view_motion.stop();
    contact_x = x;
    contact_y = y;
    touch_tail = false;
    options_ready = false;
    dirty = true;
    edge_gesture = gestures_available() ? (y >= 470  ? EdgeGesture::Bottom
                                           : x <= 10 ? EdgeGesture::Left
                                                     : EdgeGesture::None)
                                        : EdgeGesture::None;
    pointer_down = true;
    scroll_drag = false;
    picker_rail = picker_swipe = false;
    detail_drag = detail_rail = false;
    list_touch = list_swipe = tap_pending = false;
    down_x = x;
    down_y = y;
    pressed_game = navigation_selected();
    pointer_at = s.GetTicks();
    hold_felt = false;
    // A touch that begins on a gesture edge belongs to the gesture until it
    // is released: a plain release there still counts as a tap.
    if (edge_gesture != EdgeGesture::None) {
        tap_pending = true;
        return;
    }
    mark_library_press(x, y);
    if (settings.page == Page::BootWarning) {
        boot_touch_candidate = boot_displayed && !boot_touch_wait_release && x >= 0 && x < 640 && y >= 0 && y < 480;
        return;
    }
    if (o.boot_startup && (!session_initialized || boot_touch_wait_release)) {
        boot_touch_wait_release = true;
        return;
    }
    browser_list_touch = browser_rail = false;
    carousel.item_spacing = !home_menu && browse_layout() == 1 ? 158 : Carousel::spacing;
    if (picker_open && !picker_detail) {
        tap_pending = true;
        if (picker_scrollable() && y >= picker_top() + 64 && y < picker_top() + 64 + picker_height()) {
            picker_scroll_start = picker_scroll;
            if (picker_max_scroll() > 0 && x >= 572 && x < 604) {
                tap_pending = false;
                picker_rail = true;
                int local = y - picker_top() - 64, offset = picker_thumb_offset();
                picker_grab = local >= offset && local < offset + picker_thumb() ? local - offset : picker_thumb() / 2;
                scroll_cue_at = s.GetTicks();
                micro_until = scroll_cue_at + 90;
                seek_picker(y);
            } else if (x >= 40 && x < 560) picker_swipe = true;
        }
        return;
    }
    if (!probe && filter_sheet_active()) {
        // A slider follows the finger from the press; elsewhere a tap toggles on release and a drag scrolls.
        filter_touch = true;
        filter_drag = -1;
        filter_scroll_start = filter_scroll;
        filter_rail = false;
        if (filter_rail_press(x, y)) return;
        if (filter_slider_press(x, y)) return;
        tap_pending = true;
        list_touch = false;
        return;
    }
    if (!probe && !home_menu && settings.page == Page::Library && browse_layout() == 2 && y >= LibraryViewport::top &&
        y < LibraryViewport::top + LibraryViewport::height) {
        browser_list.moving = false;
        if (x >= 592 && x < 628 && navigation_count()) {
            scroll_cue_at = s.GetTicks();
            browser_rail = true;
            browser_grab =
                y >= browser_list.thumb_top(navigation_count()) &&
                        y < browser_list.thumb_top(navigation_count()) + browser_list.thumb(navigation_count())
                    ? y - browser_list.thumb_top(navigation_count())
                    : browser_list.thumb(navigation_count()) / 2;
            browser_list.seek(y, browser_grab, navigation_count());
            model.selected = size_t(browser_list.first());
            carousel.reset(model.selected);
            return;
        }
        browser_list_touch = true;
        browser_scroll_start = browser_list.scroll;
        tap_pending = true;
        return;
    }
    if (!probe && home_menu && settings.page == Page::Library && preferences.home_layout == 1) {
        tap_pending = true;
        return;
    }
    constexpr int arrow_width = RailGeometry::left, right_arrow_x = RailGeometry::left + RailGeometry::width;
    if (!probe && settings.page == Page::Library && (home_menu || browse_layout() != 2) && navigation_count() &&
        y >= 409 && y < 480) {
        if (x < arrow_width) {
            carousel.sync_input(pointer_at);
            hold.press(4000, -1, pointer_at);
            navigate(-1);
            return;
        }
        if (x >= right_arrow_x) {
            carousel.sync_input(pointer_at);
            hold.press(4000, 1, pointer_at);
            navigate(1);
            return;
        }
    }
    bool horizontal_seek = x >= arrow_width && x < right_arrow_x;
    if (!probe && settings.page == Page::Library && navigation_count() &&
        ((y >= LibraryViewport::top && y < LibraryViewport::top + LibraryViewport::height) ||
         (horizontal_seek && y >= 409 && y < 480))) {
        if (y >= 409) carousel.seek_down(x, y, navigation_count(), pointer_at);
        else carousel.down(x, y, false, pointer_at);
        if (carousel.range) {
            auto selected = carousel.drag(x, navigation_count());
            if (home_menu) home_selected = selected;
            else if (systems_menu) systems_selected = selected;
            else {
                model.selected = selected;
                browser_list.reveal(selected, navigation_count());
            }
            selected_at = s.GetTicks();
            dirty = true;
        }
        return;
    }
    if (picker_open && picker_detail && y >= 164 && y < 398) {
        detail_drag = true;
        detail_rail = x >= 588;
        gesture_scroll = detail_scroll;
        if (detail_rail) scroll_detail_rail(y);
        return;
    }
    if (!probe && settings.page == Page::ShaderPicker && shader_pointer_down(x, y)) return;
    if (!probe && settings.page != Page::Library) {
        bool slider = !picker_open && slider_hit(x, y);
        bool rail = !picker_open && list_count() > 0 && x >= 596 && y >= list_top && y < list_top + list_height;
        if (!slider && !rail) {
            tap_pending = true;
            list_touch = !picker_open && list_count() > 0 && y >= list_top && y < list_top + list_height;
            sync_list();
            gesture_scroll = list_scroll;
            return;
        }
    }
    if (!probe && !home_menu && settings.page == Page::Library && y < LibraryViewport::top) {
        tap_pending = true;
        return;
    }
    touch(x, y);
}

void App::pointer_move(int x, int y) {
    if (chat_open) {
        if (pointer_down) {
            contact_x = x;
            contact_y = y;
            if (edge_gesture != EdgeGesture::None) edge_gesture_move(x, y);
            else chat_drag(x, y);
            dirty = true;
        }
        return;
    }
    x += view_touch_dx;
    y += view_touch_dy;
    if (!pointer_down) return;
    if (contact_x != x || contact_y != y) {
        contact_x = x;
        contact_y = y;
        dirty = true;
    }
    if (edge_gesture_move(x, y)) return;
    if (!library_control_active(LibraryPress::LeftArrow, rail_left_bounds()) &&
        !library_control_active(LibraryPress::RightArrow, rail_right_bounds()))
        hold.release(4000);
    if (settings.page == Page::BootWarning) return;
    if (shader_drag) {
        if (std::abs(y - down_y) > 12) shader_carousel.moved = true;
        int index = int(shader_carousel.drag(x, shader_choices.size()));
        if (index != settings.focus) {
            settings.focus = index;
            selected_at = s.GetTicks();
            haptic_detent();
        }
        dirty = true;
        return;
    }
    if (picker_open && picker_rail) {
        seek_picker(y);
        return;
    }
    if (picker_open && picker_swipe) {
        if (std::abs(y - down_y) > 8) {
            tap_pending = false;
            const int row = picker_scroll / picker_stride();
            scroll_picker(picker_scroll_start + down_y - y);
            if (picker_scroll / picker_stride() != row) haptic_detent();
        }
        return;
    }
    if (detail_drag) {
        if (detail_rail) scroll_detail_rail(y);
        else detail_scroll = std::clamp(gesture_scroll + (down_y - y) / 28, 0, detail_max_scroll());
        dirty = true;
        return;
    }
    if (filter_touch && filter_sheet_active()) {
        if (filter_rail) filter_seek(y);
        else if (filter_drag >= 0) filter_slider_drag(x);
        else if (std::abs(y - down_y) > 8) {
            tap_pending = false;
            filter_drag_scroll(y);
        }
        return;
    }
    if (browser_rail) {
        browser_list.seek(y, browser_grab, navigation_count());
        model.selected = size_t(browser_list.first());
        carousel.reset(model.selected);
        dirty = true;
        return;
    }
    if (browser_list_touch) {
        if (std::abs(y - down_y) > 12) {
            tap_pending = false;
            browser_list.scroll = browser_scroll_start + down_y - y;
            browser_list.clamp(navigation_count());
            if (navigation_count()) {
                if (size_t(browser_list.first()) != model.selected) haptic_detent();
                model.selected = size_t(browser_list.first());
                carousel.reset(model.selected);
            }
            dirty = true;
        }
        return;
    }
    if (list_touch) {
        if (std::abs(y - down_y) > 8) list_swipe = true;
        if (list_swipe) {
            tap_pending = false;
            const int row = list_scroll / list_stride;
            list_scroll = std::clamp(gesture_scroll + down_y - y, 0, list_max_scroll());
            if (list_scroll / list_stride != row) haptic_detent();
            dirty = true;
        }
        return;
    }
    if (tap_pending && (std::abs(x - down_x) > 12 || std::abs(y - down_y) > 12)) tap_pending = false;
    if (scroll_drag) {
        scroll_to(y);
        return;
    }
    if (carousel.dragging) {
        if (std::abs(y - down_y) > 12) carousel.moved = true;
        auto selected = carousel.drag(x, navigation_count());
        if (selected != navigation_selected()) {
            if (home_menu) home_selected = selected;
            else if (systems_menu) systems_selected = selected;
            else {
                model.selected = selected;
                browser_list.reveal(selected, navigation_count());
            }
            selected_at = s.GetTicks();
            haptic_detent();
        }
        dirty = true;
    } else if (brightness_drag || volume_drag) {
        const int before = volume_drag ? hardware.volume_percent : system_preferences.brightness;
        if (volume_drag)
            change_volume(
                std::clamp(int(std::round((x - sound_track_x) * 20.0 / sound_track_width)) * 5, 0, 100));
        else touch(std::clamp(x, 44, 596), std::clamp(y, slider_band().first, slider_band().second));
        if ((volume_drag ? hardware.volume_percent : system_preferences.brightness) != before) haptic_detent();
    } else if (probe) {
        touch_x = x;
        touch_y = y;
        last_input = "Touch " + std::to_string(x) + ", " + std::to_string(y);
        dirty = true;
    }
}

void App::pointer_end(int x, int y) {
    if (chat_open) {
        if (pointer_down) {
            if (edge_gesture != EdgeGesture::None) {
                edge_gesture = EdgeGesture::None;
                chat_press(down_x, down_y);
            }
            chat_release();
            pointer_down = false;
            pointer_source = PointerSource::None;
            dirty = true;
        }
        return;
    }
    hold.release(4000);
    x += view_touch_dx;
    y += view_touch_dy;
    if (!pointer_down) return;
    edge_gesture = EdgeGesture::None;
    release_at = s.GetTicks();
    touch_tail = preferences.touch_indicator && preferences.animations;
    dirty = true;
    pointer_down = false;
    pointer_source = PointerSource::None;
    scroll_drag = false;
    picker_rail = picker_swipe = false;
    detail_drag = detail_rail = false;
    brightness_drag = false;
    volume_drag = false;
    filter_touch = false;
    filter_rail = false;
    filter_drag = -1;
    if (o.boot_startup && boot_touch_wait_release) {
        boot_touch_wait_release = false;
        boot_touch_candidate = false;
        return;
    }
    if (settings.page == Page::BootWarning) {
        bool accept = boot_touch_candidate && !boot_touch_wait_release;
        boot_touch_candidate = false;
        boot_touch_wait_release = false;
        if (accept) continue_boot();
        return;
    }
    if (shader_drag) {
        shader_drag = false;
        shader_carousel.dragging = false;
        if (shader_carousel.moved || shader_carousel.range) {
            focus_shader(settings.focus);
            return;
        }
        const int index = int(shader_carousel.tapped(x, shader_choices.size()));
        const bool apply =
            index == settings.focus && std::abs(shader_carousel.position - index) < .08 && y >= 128 && y < 330;
        focus_shader(index);
        if (apply) choose_shader();
        return;
    }
    if (browser_rail) {
        browser_rail = false;
        return;
    }
    if (browser_list_touch) {
        browser_list_touch = false;
        bool tap = tap_pending && std::abs(x - down_x) <= 12 && std::abs(y - down_y) <= 12;
        tap_pending = false;
        int hit = browser_list.row_at(y, model.games.size());
        if (tap && x >= 24 && x < 592 && hit >= 0) {
            size_t index = size_t(hit);
            select_game(index, false);
            if (index == pressed_game) {
                if (s.GetTicks() - pointer_at >= 600) open_game_options();
                else launch();
            }
        }
        return;
    }
    bool tapped = tap_pending && std::abs(x - down_x) <= 12 && std::abs(y - down_y) <= 12;
    tap_pending = list_touch = list_swipe = false;
    if (tapped) {
        touch(x, y);
        brightness_drag = volume_drag = scroll_drag = false;
        return;
    }
    if (carousel.dragging) {
        carousel.dragging = false;
        if (carousel.moved || carousel.range) {
            select_navigation(navigation_selected());
            return;
        }
        long raw = !home_menu && browse_layout() == 1
                       ? cover_hit(x, y)
                       : long(std::llround(carousel.position + (x - 320) / carousel.item_spacing));
        if (raw < 0 || raw >= long(navigation_count())) {
            carousel.select(navigation_selected(), s.GetTicks());
            dirty = true;
            return;
        }
        size_t index = size_t(raw);
        bool launch_selected = index == pressed_game && std::abs(carousel.position - index) < 0.08 &&
                               y >= LibraryViewport::top && y < LibraryViewport::top + LibraryViewport::height;
        select_navigation(index);
        if (launch_selected) {
            if (!home_menu && !systems_menu && s.GetTicks() - pointer_at >= 600) open_game_options();
            else launch();
        }
    }
}

bool App::mouse_position(Uint32 window, int& x, int& y) {
    (void)x;
    if (o.preview) {
        if (y < 480 && !pointer_down) return false;
        y -= 480;
        return true;
    }
    return surfaces.size() > 1 && window == surfaces[1].id;
}

void App::raw_touch(const TouchPoint& point, Uint32 now) {
    SoundAction sound_action(*this, 2);
    if (!departure_action.empty()) return;
    if (o.boot_startup && boot_touch_wait_release && (point.kind == 2 || point.kind == 3)) {
        boot_touch_wait_release = false;
        boot_touch_candidate = false;
        cancel_navigation();
        return;
    }
    if (o.game_menu && point.kind != 1)
        menu_input = "touch " +
                     std::string(point.kind == 0   ? "down"
                                 : point.kind == 2 ? "up"
                                                   : "cancel") +
                     " at " + std::to_string(point.x) + "," + std::to_string(point.y);
    if (point.kind == 0) {
        if (pointer_down && pointer_source != PointerSource::Raw) return;
        pointer_source = PointerSource::Raw;
    } else if (pointer_source != PointerSource::Raw) return;
    raw_touch_seen = true;
    raw_touch_at = now;
    last_activity = now;
    auto before = settings.page;
    if (point.kind == 0) pointer_begin(point.x, point.y);
    else if (point.kind == 1) pointer_move(point.x, point.y);
    else if (point.kind == 2) pointer_end(point.x, point.y);
    else {
        cancel_navigation();
        brightness_drag = volume_drag = false;
        carousel.select(navigation_selected(), now);
    }
    if (before != settings.page) {
        page_at = now;
        hold.clear();
    }
    dirty = true;
}

void App::event(SDL_Event e) {
    if (o.port_menu && port_idle && e.type != SDL_QUIT) return; // the port is playing
    if (o.mainline && (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) &&
        (o.preview || mainline_pads.count(e.jbutton.which)) && !mainline_controls.normalize(e))
        return;
    capture_view_for_input(e);
    if (o.game_menu) note_menu_input(e);
    auto before = settings.page;
    const int before_focus = settings.focus;
    handle_input(e);
    if (before != settings.page) {
        settings.remember(before, before_focus);
        if (before == Page::LibraryOptions) backup_requested = false;
        page_at = s.GetTicks();
        hold.clear();
        dirty = true;
    }
}

void App::handle_input(const SDL_Event& e) {
    SoundAction sound_action(*this, 2);
    if (!departure_action.empty()) return;
    if (e.type == SDL_KEYDOWN || e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYHATMOTION ||
        e.type == SDL_JOYAXISMOTION || e.type == SDL_MOUSEWHEEL)
        motion_touch = false;
    if (launching) {
        if (e.type == SDL_QUIT) quit = true;
        const char* mapped = std::getenv("RGDS_BUTTON_B");
        if ((e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_b)) ||
            (e.type == SDL_JOYBUTTONDOWN && e.jbutton.button == (mapped ? std::atoi(mapped) : 1)))
            action(3);
        return;
    }
    if (!touchscreen.available() && o.boot_startup && boot_touch_wait_release &&
        (e.type == SDL_FINGERUP || e.type == SDL_MOUSEBUTTONUP)) {
        boot_touch_wait_release = false;
        boot_touch_candidate = false;
        cancel_navigation();
        return;
    }
    if (e.type == SDL_KEYUP && boot_blocked_keys.erase(e.key.keysym.sym)) return;
    if (e.type == SDL_JOYBUTTONUP && boot_blocked_buttons.erase({e.jbutton.which, e.jbutton.button})) return;
    if (e.type == SDL_KEYDOWN && boot_blocked_keys.count(e.key.keysym.sym)) return;
    if (e.type == SDL_JOYBUTTONDOWN && boot_blocked_buttons.count({e.jbutton.which, e.jbutton.button})) return;
    if (e.type == SDL_JOYHATMOTION && menu_held_hats.count(e.jhat.which)) {
        if (e.jhat.value == SDL_HAT_CENTERED) menu_held_hats.erase(e.jhat.which);
        return;
    }
    bool anbernic_key = e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_HOME || e.key.keysym.sym == SDLK_AC_HOME ||
                                                  e.key.keysym.sym == SDLK_AC_BACK || e.key.keysym.sym == SDLK_MENU);
    bool anbernic_button = e.type == SDL_JOYBUTTONDOWN && joystick_source(e.jbutton.button) == control_menu;
    if (chat_open) {
        if (anbernic_key || anbernic_button) {
            chat_action(3);
            return;
        }
        if (chat_event(e)) return;
    }
    if (!o.game_menu && session_initialized && settings.page != Page::BootWarning &&
        (anbernic_key || anbernic_button)) {
        // The Anbernic button opens the game switcher; the Home key the game search.
        if (anbernic_button) launcher_switcher_shortcut();
        else if (!e.key.repeat) launcher_search_shortcut();
        return;
    }
    if (settings.page == Page::Controls && control_capture) {
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            controls_action(3);
            return;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym != SDLK_VOLUMEUP &&
            e.key.keysym.sym != SDLK_VOLUMEDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_HOME || e.key.keysym.sym == SDLK_AC_HOME ||
                e.key.keysym.sym == SDLK_MENU) {
                controls_action(3);
                return;
            }
            const std::map<SDL_Keycode, int> keys = {
                {SDLK_a, 0},      {SDLK_b, 1},   {SDLK_x, 2},  {SDLK_y, 3},    {SDLK_l, 4},         {SDLK_r, 5},
                {SDLK_RETURN, 6}, {SDLK_TAB, 7}, {SDLK_UP, 8}, {SDLK_DOWN, 9}, {SDLK_LEFT, 10},     {SDLK_RIGHT, 11},
                {SDLK_q, 12},     {SDLK_e, 13},  {SDLK_z, 14}, {SDLK_c, 15},   {SDLK_BACKSPACE, 16}};
            auto found = keys.find(e.key.keysym.sym);
            if (found != keys.end()) {
                capture_control(found->second);
                boot_blocked_keys.insert(e.key.keysym.sym);
            }
            return;
        }
        if (e.type == SDL_JOYBUTTONDOWN) {
            int source = joystick_source(e.jbutton.button);
            if (source == control_menu) controls_action(3);
            else if (source >= 0) {
                capture_control(source);
                boot_blocked_buttons.insert({e.jbutton.which, e.jbutton.button});
            }
            return;
        }
        if (e.type == SDL_JOYHATMOTION) {
            int source = hat_source(e.jhat.value);
            if (source >= 0) capture_control(source);
            return;
        }
    }
    if (o.boot_startup && !o.boot_warning && !session_initialized) {
        if (e.type == SDL_KEYDOWN) {
            boot_blocked_keys.insert(e.key.keysym.sym);
            return;
        }
        if (e.type == SDL_JOYBUTTONDOWN) {
            boot_blocked_buttons.insert({e.jbutton.which, e.jbutton.button});
            return;
        }
    }
    if (settings.page == Page::BootWarning) {
        bool fresh = boot_displayed && (!e.common.timestamp || int32_t(e.common.timestamp - boot_displayed_at) >= 0);
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_b) return;
            if (e.key.keysym.sym == SDLK_a || e.key.keysym.sym == SDLK_RETURN) {
                boot_blocked_keys.insert(e.key.keysym.sym);
                if (fresh && !e.key.repeat) continue_boot();
            }
            return;
        }
        if (e.type == SDL_JOYBUTTONDOWN) {
            const char* a = std::getenv("RGDS_BUTTON_A");
            if (e.jbutton.button == (a ? std::atoi(a) : 0) || e.jbutton.button == 7) {
                boot_blocked_buttons.insert({e.jbutton.which, e.jbutton.button});
                if (fresh) continue_boot();
            }
            return;
        }
    }
    if (s.GetTicks() < input_guard_until &&
        (e.type == SDL_KEYDOWN || e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYHATMOTION ||
         e.type == SDL_JOYAXISMOTION || e.type == SDL_FINGERDOWN || e.type == SDL_MOUSEBUTTONDOWN))
        return;
    const char* menu_external = std::getenv("RGDS_MENU_EXTERNAL");
    bool owned_menu_keys = embedded_menu && menu_external && std::string(menu_external) == "1";
    if (owned_menu_keys && e.type == SDL_KEYDOWN &&
        (e.key.keysym.sym == SDLK_HOME || e.key.keysym.sym == SDLK_AC_HOME || e.key.keysym.sym == SDLK_AC_BACK ||
         e.key.keysym.sym == SDLK_MENU))
        return;
    if (owned_menu_keys && (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) && e.jbutton.button == 8) return;
    if (o.game_menu && !owned_menu_keys && anbernic_key) {
        if (!e.key.repeat && menu_status["state"] != "busy" && menu_pending_token.empty()) {
            picker_open = false;
            cancel_navigation();
            settings.page = Page::QuickMenu;
            settings.focus = 0;
            dirty = true;
        }
        return;
    }
    if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_VOLUMEUP || e.key.keysym.sym == SDLK_VOLUMEDOWN)) {
        if (!volume_keys.available() && !std::getenv("RGDS_VOLUME_EXTERNAL"))
            change_volume(hardware.volume_percent + (e.key.keysym.sym == SDLK_VOLUMEUP ? 5 : -5));
        return;
    }
    if (e.type == SDL_KEYDOWN || e.type == SDL_TEXTINPUT || e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYHATMOTION ||
        e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION)
        last_activity = s.GetTicks();
    if (e.type == SDL_QUIT) {
        quit = true;
        return;
    }
    if (e.type == SDL_RENDER_DEVICE_RESET)
        throw std::runtime_error("Graphics device reset; restarting RGDS before using stale textures");
    if (e.type == SDL_RENDER_TARGETS_RESET) {
        rounded.clear();
        media.clear();
        shader_preview.clear();
        top_seen.clear();
        dirty = true;
        return;
    }
    if (e.type == SDL_KEYUP) {
        hold.release(e.key.keysym.sym);
        return;
    }
    if (e.type == SDL_JOYBUTTONUP) {
        hold.release(1000 + e.jbutton.button);
        return;
    }
    if (e.type == SDL_WINDOWEVENT) {
        if (e.window.event == SDL_WINDOWEVENT_LEAVE) {
            scroll_hover = browser_hover = false;
        }
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            hold.clear();
            bool pointer_window = o.preview || embedded_menu || !e.window.windowID ||
                                  (surfaces.size() > 1 && e.window.windowID == surfaces[1].id);
            if (pointer_source != PointerSource::Raw && pointer_window) {
                cancel_navigation();
                brightness_drag = volume_drag = false;
                carousel.select(navigation_selected(), s.GetTicks());
            }
        }
        dirty = true;
        return;
    }
    if (settings.page == Page::Keyboard && e.type == SDL_TEXTINPUT) {
        std::string value = e.text.text;
        insert_text(value);
        dirty = true;
        return;
    }
    if (e.type == SDL_KEYDOWN && settings.page == Page::Keyboard) {
        switch (e.key.keysym.sym) {
        case SDLK_ESCAPE: finish_edit(false); break;
        case SDLK_RETURN: finish_edit(true); break;
        case SDLK_BACKSPACE: erase_character(); break;
        case SDLK_DELETE: delete_character(); break;
        case SDLK_HOME:
            edit_cursor = 0;
            dirty = true;
            break;
        case SDLK_END:
            edit_cursor = edit_buffer.size();
            dirty = true;
            break;
        case SDLK_TAB: {
            auto at = keyboard_submit(keyboard_rows());
            key_row = at.first;
            key_col = at.second;
            dirty = true;
            break;
        }
        case SDLK_LEFT: action(0); break;
        case SDLK_RIGHT: action(1); break;
        case SDLK_UP: action(8); break;
        case SDLK_DOWN: action(9); break;
        default: break;
        }
        return;
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        if (!picker_open && horizontal_hold_page() && (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT))
            hold.press(e.key.keysym.sym, e.key.keysym.sym == SDLK_LEFT ? -1 : 1, s.GetTicks());
        if (!picker_open && settings.page == Page::Library && !home_menu &&
            (e.key.keysym.sym == SDLK_LEFTBRACKET || e.key.keysym.sym == SDLK_RIGHTBRACKET))
            hold.press(e.key.keysym.sym, e.key.keysym.sym == SDLK_LEFTBRACKET ? -1 : 1, s.GetTicks());
        // Up/down repeat in every list and picker; +-2 tells the poll it is vertical.
        if (e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_DOWN)
            hold.press(e.key.keysym.sym, e.key.keysym.sym == SDLK_UP ? -2 : 2, s.GetTicks());
        last_input = "Key " + std::to_string(e.key.keysym.sym);
        dirty = true;
        switch (e.key.keysym.sym) {
        case SDLK_LEFT: action(0); break;
        case SDLK_RIGHT: action(1); break;
        case SDLK_UP: action(8); break;
        case SDLK_DOWN: action(9); break;
        case SDLK_RETURN:
        case SDLK_a: action(2); break;
        case SDLK_ESCAPE:
        case SDLK_b: action(3); break;
        case SDLK_TAB:
        case SDLK_c:
            if (!o.game_menu && !picker_open) {
                settings.section = 2;
                settings.page = Page::LibraryOptions;
                settings.focus = 0;
            }
            break;
        case SDLK_F1:
            if (!o.game_menu) action(5);
            break;
        case SDLK_AC_HOME:
        case SDLK_HOME:
        case SDLK_AC_BACK:
        case SDLK_MENU: action(7); break;
        case SDLK_SLASH:
            if (settings.page == Page::Library && !home_menu && !picker_open) quick_search();
            break;
        case SDLK_r: action(6); break;
        case SDLK_s: action(7); break;
        case SDLK_x: action(11); break;
        case SDLK_y:
            if (settings.page == Page::Library || settings.page == Page::ShaderPicker ||
                settings.page == Page::SaveGallery || settings.page == Page::Maintenance)
                action(12);
            break;
        case SDLK_LEFTBRACKET:
            if (settings.page == Page::ShaderPicker || settings.page == Page::Cheats ||
                settings.page == Page::Homebrew || (settings.page == Page::Library && !home_menu))
                action(13);
            break;
        case SDLK_RIGHTBRACKET:
            if (settings.page == Page::ShaderPicker || settings.page == Page::Cheats ||
                settings.page == Page::Homebrew || (settings.page == Page::Library && !home_menu))
                action(14);
            break;
        case SDLK_q:
            if ((settings.page == Page::Library && !home_menu) || (settings.page == Page::Keyboard && edit_field == 4))
                action(15);
            break;
        case SDLK_e:
            if ((settings.page == Page::Library && !home_menu) || (settings.page == Page::Keyboard && edit_field == 4))
                action(16);
            break;
        default: break;
        }
    } else if (e.type == SDL_JOYBUTTONDOWN) {
        if (((settings.page == Page::Library && !home_menu) || (settings.page == Page::Keyboard && edit_field == 4)) &&
            (joystick_source(e.jbutton.button) == 12 || joystick_source(e.jbutton.button) == 13)) {
            action(joystick_source(e.jbutton.button) == 12 ? 15 : 16);
            return;
        }

        // The shoulders step shader families and the cheats tabs; elsewhere they stand in for Left/Right.
        if ((settings.page == Page::ShaderPicker || settings.page == Page::Cheats || settings.page == Page::Homebrew) &&
            (e.jbutton.button == 4 || e.jbutton.button == 5)) {
            action(e.jbutton.button == 4 ? 13 : 14);
            return;
        }
        if (!picker_open && horizontal_hold_page() && (e.jbutton.button == 4 || e.jbutton.button == 5))
            hold.press(1000 + e.jbutton.button, e.jbutton.button == 4 ? -1 : 1, s.GetTicks());
        const char* a = std::getenv("RGDS_BUTTON_A");
        const char* b = std::getenv("RGDS_BUTTON_B");
        const char* select = std::getenv("RGDS_BUTTON_SETTINGS");
        if (e.jbutton.button == (a ? std::atoi(a) : 0)) action(2);
        else if (e.jbutton.button == (b ? std::atoi(b) : 1)) action(3);
        else if (e.jbutton.button == 4) action(0);
        else if (e.jbutton.button == 5) action(1);
        else if (e.jbutton.button == (select ? std::atoi(select) : 6)) action(7);
        else if (e.jbutton.button == 7) action(4);
        else if (e.jbutton.button == 8) action(7);
        else if (e.jbutton.button == 3) action(11);
        else if (e.jbutton.button == 2 && (settings.page == Page::Library || settings.page == Page::ShaderPicker ||
                                           settings.page == Page::Keyboard || settings.page == Page::SaveGallery ||
                                           settings.page == Page::Maintenance || settings.page == Page::Bluetooth ||
                                           settings.page == Page::Homebrew))
            action(12);
    } else if (e.type == SDL_JOYHATMOTION) {
        hold.release(2000);
        hold.release(2001);
        if (horizontal_hold_page() && (e.jhat.value & (SDL_HAT_LEFT | SDL_HAT_RIGHT)))
            hold.press(2000, e.jhat.value & SDL_HAT_LEFT ? -1 : 1, s.GetTicks());
        else if (!(e.jhat.value & (SDL_HAT_LEFT | SDL_HAT_RIGHT)) && (e.jhat.value & (SDL_HAT_UP | SDL_HAT_DOWN)))
            hold.press(2001, e.jhat.value & SDL_HAT_UP ? -2 : 2, s.GetTicks());
        if (e.jhat.value & SDL_HAT_LEFT) action(0);
        else if (e.jhat.value & SDL_HAT_RIGHT) action(1);
        else if (e.jhat.value & SDL_HAT_UP) action(8);
        else if (e.jhat.value & SDL_HAT_DOWN) action(9);
    } else if (e.type == SDL_JOYAXISMOTION) {
        if (e.jaxis.axis == 0) {
            if (std::abs(int(e.jaxis.value)) < 10000) hold.release(3000);
            else if (horizontal_hold_page()) hold.press(3000, e.jaxis.value < 0 ? -1 : 1, s.GetTicks());
        }
        if (e.jaxis.axis == 1) {
            if (std::abs(int(e.jaxis.value)) < 10000) hold.release(3001);
            else hold.press(3001, e.jaxis.value < 0 ? -2 : 2, s.GetTicks());
        }
        auto now = s.GetTicks();
        if (std::abs(static_cast<int>(e.jaxis.value)) > 16000 && now - last_axis > 180) {
            if (e.jaxis.axis == 0) action(e.jaxis.value < 0 ? 0 : 1);
            else if (e.jaxis.axis == 1) action(e.jaxis.value < 0 ? 8 : 9);
            last_axis = now;
        }
    } else if (e.type == SDL_MOUSEMOTION && !pointer_down) {
        int x = e.motion.x, y = e.motion.y;
        bool in_bottom = mouse_position(e.motion.windowID, x, y);
        bool hover =
            in_bottom && (picker_open ? (picker_scrollable() && picker_max_scroll() > 0 && x >= 572 && x < 604 &&
                                         y >= picker_top() + 64 && y < picker_top() + 64 + picker_height())
                                      : settings.page != Page::Library && list_max_scroll() > 0 && x >= 596 &&
                                            y >= list_top && y < list_top + list_height);
        bool browser = in_bottom && !picker_open && settings.page == Page::Library && !home_menu &&
                       browse_layout() == 2 && browser_list.maximum(navigation_count()) > 0 && x >= 596 && x < 628 &&
                       y >= LibraryViewport::top && y < LibraryViewport::top + LibraryViewport::height;
        if (hover != scroll_hover || browser != browser_hover) {
            scroll_hover = hover;
            browser_hover = browser;
            dirty = true;
        }
    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.which != SDL_TOUCH_MOUSEID &&
               e.button.button == SDL_BUTTON_LEFT) {
        auto now = s.GetTicks();
        if (touchscreen.available() || (raw_touch_seen && now - raw_touch_at < 250) ||
            (sdl_touch_seen && now - sdl_touch_at < 250))
            return;
        int x = e.button.x, y = e.button.y;
        if ((pointer_source == PointerSource::None || pointer_source == PointerSource::Mouse) &&
            mouse_position(e.button.windowID, x, y)) {
            if (settings.page == Page::BootWarning && e.common.timestamp &&
                int32_t(e.common.timestamp - boot_displayed_at) < 0)
                boot_touch_wait_release = true;
            pointer_source = PointerSource::Mouse;
            pointer_begin(x, y);
        }
    } else if (e.type == SDL_MOUSEMOTION && e.motion.which != SDL_TOUCH_MOUSEID && pointer_down &&
               pointer_source == PointerSource::Mouse && (e.motion.state & SDL_BUTTON_LMASK)) {
        int x = e.motion.x, y = e.motion.y;
        if (mouse_position(e.motion.windowID, x, y)) pointer_move(x, y);
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.which != SDL_TOUCH_MOUSEID) {
        if (pointer_source != PointerSource::Mouse) return;
        int x = e.button.x, y = e.button.y;
        if (e.button.windowID == 0) {
            x = contact_x;
            y = contact_y;
        } else if (!mouse_position(e.button.windowID, x, y)) return;
        pointer_end(x, y);
    } else if (e.type == SDL_MOUSEWHEEL && !probe) {
        if (picker_open) {
            if (picker_scrollable()) {
                scroll_picker(picker_scroll - e.wheel.y * 40);
            } else if (e.wheel.y) picker_action(e.wheel.y > 0 ? 8 : 9);
            return;
        }
        if (settings.page == Page::Library) navigate(std::clamp(e.wheel.x ? e.wheel.x : -e.wheel.y, -3, 3));
        else if (list_count() > 0) {
            sync_list();
            list_scroll = std::clamp(list_scroll - e.wheel.y * 40, 0, list_max_scroll());
            dirty = true;
        }
    } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
        // Compositor coordinates may span both displays. The calibrated evdev
        // source owns physical touch for its entire lifetime, including idle
        // gaps; SDL must not win a race before the next raw poll.
        auto now = s.GetTicks();
        if (touchscreen.available() || pointer_source == PointerSource::Raw ||
            (pointer_source == PointerSource::None && raw_touch_seen && now - raw_touch_at < 80))
            return;
        int x = std::clamp(static_cast<int>(e.tfinger.x * 640), 0, 639),
            y = static_cast<int>(e.tfinger.y * (o.preview ? 960 : 480));
        if (o.preview) {
            if (y < 480 && !pointer_down) return;
            y -= 480;
        }
        // KMS associates touch with the focused window, which can be the top
        // panel. Physical touch always belongs to the lower DS screen.
        else if (!embedded_menu && surfaces.size() < 2)
            return;
        y = std::clamp(y, 0, 479);
        if (e.type == SDL_FINGERDOWN) {
            if (pointer_down) return;
            if (settings.page == Page::BootWarning && e.common.timestamp &&
                int32_t(e.common.timestamp - boot_displayed_at) < 0)
                boot_touch_wait_release = true;
            active_finger = e.tfinger.fingerId;
            pointer_source = PointerSource::SDL;
            sdl_touch_seen = true;
            sdl_touch_at = now;
            pointer_begin(x, y);
        } else if (pointer_source == PointerSource::SDL && e.tfinger.fingerId == active_finger) {
            sdl_touch_at = now;
            if (e.type == SDL_FINGERUP) pointer_end(x, y);
            else pointer_move(x, y);
        }
    }
    if (dirty && probe) std::cout << last_input << std::endl;
}
