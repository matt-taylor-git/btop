/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <signal.h>

#if defined(_WIN32)
#include <windows.h>
#include "windows/signal_compat.hpp"
#else
#include <sys/select.h>
#include <unistd.h>
#endif

#include "btop_input.hpp"
#include "btop_tools.hpp"
#include "btop_config.hpp"
#include "btop_shared.hpp"
#include "btop_menu.hpp"
#include "btop_draw.hpp"

using namespace Tools;
using namespace std::literals; // for operator""s
namespace rng = std::ranges;

namespace Input {

	//* Map for translating key codes to readable values
	const std::unordered_map<string, string> Key_escapes = {
		{"\033",	"escape"},
		{"\x03",	"q"},
		{"\x12",	"ctrl_r"},
		{"\n",		"enter"},
		{" ",		"space"},
		{"\x7f",	"backspace"},
		{"\x08",	"backspace"},
		{"[A", 		"up"},
		{"OA",		"up"},
		{"[B", 		"down"},
		{"OB",		"down"},
		{"[D", 		"left"},
		{"OD",		"left"},
		{"[C", 		"right"},
		{"OC",		"right"},
		{"[2~",		"insert"},
		{"[4h",		"insert"},
		{"[3~",		"delete"},
		{"[P",		"delete"},
		{"[H",		"home"},
		{"[1~",		"home"},
		{"[F",		"end"},
		{"[4~",		"end"},
		{"[5~",		"page_up"},
		{"[6~",		"page_down"},
		{"\t",		"tab"},
		{"[Z",		"shift_tab"},
		{"OP",		"f1"},
		{"OQ",		"f2"},
		{"OR",		"f3"},
		{"OS",		"f4"},
		{"[15~",	"f5"},
		{"[17~",	"f6"},
		{"[18~",	"f7"},
		{"[19~",	"f8"},
		{"[20~",	"f9"},
		{"[21~",	"f10"},
		{"[23~",	"f11"},
		{"[24~",	"f12"}
	};

	sigset_t signal_mask;
	std::atomic<bool> polling (false);
	array<int, 2> mouse_pos;
	std::unordered_map<string, Mouse_loc> mouse_mappings;
	bool dragging_scroll;

	deque<string> history(50, "");
	string old_filter;
	string input;

#if defined(_WIN32)
	string utf16_units_to_utf8(const wchar_t* units, const int count) {
		if (units == nullptr or count <= 0) return {};
		char buffer[8]{};
		const int written = WideCharToMultiByte(CP_UTF8, 0, units, count, buffer, sizeof(buffer), nullptr, nullptr);
		return written > 0 ? string(buffer, written) : string{};
	}

	uint16_t utf16_code_unit(const wchar_t ch) {
		return static_cast<uint16_t>(ch);
	}

	bool is_high_surrogate(const uint16_t ch) {
		return ch >= 0xD800 and ch <= 0xDBFF;
	}

	bool is_low_surrogate(const uint16_t ch) {
		return ch >= 0xDC00 and ch <= 0xDFFF;
	}

	string windows_key_to_input(const WORD virtual_key, const wchar_t unicode_char, const DWORD control_key_state, const bool key_down) {
		static uint16_t pending_high_surrogate = 0;
		if (!key_down) return {};
		switch (virtual_key) {
			case VK_RETURN: return "\n";
			case VK_ESCAPE: return "\033";
			case VK_BACK: return "\x7f";
			case VK_TAB: return (control_key_state & SHIFT_PRESSED) ? "\033[Z" : "\t";
			case VK_UP: return "\033[A";
			case VK_DOWN: return "\033[B";
			case VK_LEFT: return "\033[D";
			case VK_RIGHT: return "\033[C";
			case VK_INSERT: return "\033[2~";
			case VK_DELETE: return "\033[3~";
			case VK_HOME: return "\033[H";
			case VK_END: return "\033[F";
			case VK_PRIOR: return "\033[5~";
			case VK_NEXT: return "\033[6~";
			case VK_F1: return "\033OP";
			case VK_F2: return "\033OQ";
			case VK_F3: return "\033OR";
			case VK_F4: return "\033OS";
			case VK_F5: return "\033[15~";
			case VK_F6: return "\033[17~";
			case VK_F7: return "\033[18~";
			case VK_F8: return "\033[19~";
			case VK_F9: return "\033[20~";
			case VK_F10: return "\033[21~";
			case VK_F11: return "\033[23~";
			case VK_F12: return "\033[24~";
			default: break;
		}
		if (unicode_char == 0x03) {
			pending_high_surrogate = 0;
			return "\x03";
		}
		if (unicode_char == 0x12) {
			pending_high_surrogate = 0;
			return "\x12";
		}
		const uint16_t code_unit = utf16_code_unit(unicode_char);
		if (is_high_surrogate(code_unit)) {
			pending_high_surrogate = code_unit;
			return {};
		}
		if (is_low_surrogate(code_unit)) {
			if (pending_high_surrogate == 0) return {};
			const wchar_t pair[] = {static_cast<wchar_t>(pending_high_surrogate), unicode_char};
			pending_high_surrogate = 0;
			return utf16_units_to_utf8(pair, 2);
		}
		pending_high_surrogate = 0;
		if (code_unit >= L' ') return utf16_units_to_utf8(&unicode_char, 1);
		return {};
	}

	string windows_mouse_to_input(const DWORD event_flags, const DWORD button_state, const SHORT x, const SHORT y) {
		const int col = x + 1;
		const int line = y + 1;
		if (event_flags == MOUSE_WHEELED) {
			return fmt::format("\033[<{};{};{}M", static_cast<SHORT>(HIWORD(button_state)) > 0 ? 64 : 65, col, line);
		}
		if (event_flags == MOUSE_MOVED and (button_state & FROM_LEFT_1ST_BUTTON_PRESSED)) {
			return fmt::format("\033[<32;{};{}M", col, line);
		}
		if ((button_state & FROM_LEFT_1ST_BUTTON_PRESSED) and event_flags != MOUSE_MOVED and event_flags != MOUSE_WHEELED) {
			return fmt::format("\033[<0;{};{}M", col, line);
		}
		if (event_flags == 0 and button_state == 0) {
			return fmt::format("\033[<0;{};{}m", col, line);
		}
		return {};
	}

	namespace {
		deque<string> pending_inputs;

		HANDLE input_handle() {
			static HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
			return handle;
		}

		string key_event_to_input(const KEY_EVENT_RECORD& key) {
			return windows_key_to_input(key.wVirtualKeyCode, key.uChar.UnicodeChar, key.dwControlKeyState, key.bKeyDown);
		}

		string mouse_event_to_input(const MOUSE_EVENT_RECORD& mouse) {
			return windows_mouse_to_input(mouse.dwEventFlags, mouse.dwButtonState, mouse.dwMousePosition.X, mouse.dwMousePosition.Y);
		}
	}

	bool poll(const uint64_t timeout) {
		atomic_lock lck(polling);
		input.clear();
		if (!pending_inputs.empty()) {
			input = pending_inputs.front();
			pending_inputs.pop_front();
			return true;
		}
		const HANDLE handle = input_handle();
		if (handle == INVALID_HANDLE_VALUE or handle == nullptr) return false;

		const auto start = GetTickCount64();
		while (timeout == std::numeric_limits<uint64_t>::max() or GetTickCount64() - start < timeout) {
			DWORD wait_ms = INFINITE;
			if (timeout != std::numeric_limits<uint64_t>::max()) {
				const auto elapsed = GetTickCount64() - start;
				if (elapsed >= timeout) break;
				wait_ms = static_cast<DWORD>(std::min<uint64_t>(50, timeout - elapsed));
			}
			const DWORD wait = WaitForSingleObject(handle, wait_ms);
			if (wait != WAIT_OBJECT_0) return false;

			INPUT_RECORD record{};
			DWORD read = 0;
			if (!ReadConsoleInputW(handle, &record, 1, &read) or read == 0) return false;
			if (record.EventType == KEY_EVENT) {
				input = key_event_to_input(record.Event.KeyEvent);
				if (!input.empty()) {
					const auto repeat_count = std::max<WORD>(1, record.Event.KeyEvent.wRepeatCount);
					for (WORD i = 1; i < repeat_count; ++i) pending_inputs.push_back(input);
				}
			}
			else if (record.EventType == MOUSE_EVENT and !Config::getB("disable_mouse")) input = mouse_event_to_input(record.Event.MouseEvent);
			else if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) Global::resized = true;
			if (!input.empty()) return true;
		}
		return false;
	}
#else
	bool poll(const uint64_t timeout) {
		atomic_lock lck(polling);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		struct timespec wait;
		struct timespec *waitptr = nullptr;

		if(timeout != std::numeric_limits<uint64_t>::max()) {
			wait.tv_sec = timeout / 1000;
			wait.tv_nsec = (timeout % 1000) * 1000000;
			waitptr = &wait;
		}

		if(pselect(STDIN_FILENO + 1, &fds, nullptr, nullptr, waitptr, &signal_mask) > 0) {
			input.clear();
			char buf[1024];
			ssize_t count = 0;
			while((count = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
				input.append(std::string_view(buf, count));
			}

			return true;
		}

		return false;
	}

#endif

	string normalize_raw_input(string key, const bool map_mouse_actions) {
		if (key.empty()) return {};

		//? Remove escape code prefix if present
		if (key.length() > 1 and key.at(0) == Fx::e.at(0)) {
			key.erase(0, 1);
		}

		//? Detect if input is a mouse event.
		if (key.starts_with("[<")) {
			std::string_view key_view = key;
			string mouse_event;
			if (key_view.starts_with("[<0;") and key_view.find('M') != std::string_view::npos) {
				mouse_event = "mouse_click";
				key_view.remove_prefix(4);
			}
			else if (key_view.starts_with("[<32;")) {
				mouse_event = "mouse_drag";
				key_view.remove_prefix(5);
			}
			else if (key_view.starts_with("[<0;") and key_view.ends_with('m')) {
				mouse_event = "mouse_release";
				key_view.remove_prefix(4);
			}
			else if (key_view.starts_with("[<64;")) {
				mouse_event = "mouse_scroll_up";
				key_view.remove_prefix(5);
			}
			else if (key_view.starts_with("[<65;")) {
				mouse_event = "mouse_scroll_down";
				key_view.remove_prefix(5);
			}
			else {
				return {};
			}

			if (map_mouse_actions and Config::getB("proc_filtering")) {
				return mouse_event == "mouse_click" ? mouse_event : string{};
			}

			try {
				const auto delim = key_view.find(';');
				if (delim == std::string_view::npos) return {};
				const auto end = key_view.find_first_of("Mm", delim + 1);
				mouse_pos[0] = stoi((string)key_view.substr(0, delim));
				mouse_pos[1] = stoi((string)key_view.substr(delim + 1, end - delim - 1));
			}
			catch (const std::invalid_argument&) { return {}; }
			catch (const std::out_of_range&) { return {}; }

			key = mouse_event;
			if (map_mouse_actions and (key == "mouse_click" or key == "mouse_drag")) {
				const auto& [col, line] = mouse_pos;
				for (const auto& [mapped_key, pos] : (Menu::active ? Menu::mouse_mappings : mouse_mappings)) {
					if (col >= pos.col and col < pos.col + pos.width and line >= pos.line and line < pos.line + pos.height) {
						key = mapped_key;
						break;
					}
				}
			}
		}
		else if (auto it = Key_escapes.find(key); it != Key_escapes.end()) {
			key = it->second;
		}
		else if (ulen(key) > 1) {
			key.clear();
		}

		return key;
	}

	string get() {
		string key = normalize_raw_input(input);
		if (not key.empty()) {
			history.push_back(key);
			history.pop_front();
		}
		return key;
	}
	string wait() {
		while(not poll(std::numeric_limits<uint64_t>::max())) {}
		return get();
	}

	void interrupt() {
#if defined(_WIN32)
		INPUT_RECORD record{};
		record.EventType = KEY_EVENT;
		record.Event.KeyEvent.bKeyDown = FALSE;
		DWORD written = 0;
		WriteConsoleInputW(input_handle(), &record, 1, &written);
#else
		kill(getpid(), SIGUSR1);
#endif
	}

	void clear() {
		// do not need it, actually
	}

	void process(const std::string_view key) {
		if (key.empty()) return;
		try {
			auto filtering = Config::getB("proc_filtering");
			auto vim_keys = Config::getB("vim_keys");
			auto help_key = (vim_keys ? "H" : "h");
			auto kill_key = (vim_keys ? "K" : "k");
			//? Global input actions
			if (not filtering) {
				bool keep_going = false;
				if (key == "q") {
					clean_quit(0);
				}
				else if (is_in(key, "escape", "m")) {
					Menu::show(Menu::Menus::Main);
					return;
				}
				else if (is_in(key, "f1", "?", help_key)) {
					Menu::show(Menu::Menus::Help);
					return;
				}
				else if (is_in(key, "f2", "o")) {
					Menu::show(Menu::Menus::Options);
					return;
				}
				else if (key.size() == 1 and isint(key)) {
					auto intKey = std::atoi(key.data());
				#ifdef GPU_SUPPORT
					static const array<string, 10> boxes = {"gpu5", "cpu", "mem", "net", "proc", "gpu0", "gpu1", "gpu2", "gpu3", "gpu4"};
					if ((intKey == 0 and Gpu::count < 5) or (intKey >= 5 and intKey - 4 > Gpu::count))
						return;
				#else
				static const array<string, 10> boxes = {"", "cpu", "mem", "net", "proc"};
					if (intKey == 0 or intKey > 4)
						return;
				#endif
					atomic_wait(Runner::active);

					if (not Config::toggle_box(boxes.at(intKey))) {
						Menu::show(Menu::Menus::SizeError);
						return;
					}
					Config::current_preset.reset();
					Draw::calcSizes();
					Draw::update_clock(true);
					Runner::run("all", false, true);
					return;
				}
				else if (is_in(key, "p", "P") and Config::getS("disable_presets") != "All") {
					if (Config::getS("disable_presets") == "Default" and Config::preset_list.size() <= 1) return;
					atomic_wait(Runner::active);
					const auto old_preset = Config::current_preset;
					const int first_preset = (Config::getS("disable_presets") == "Default") ? 1 : 0;
					if (Config::getS("disable_presets") == "Custom") Config::current_preset = 0;
					else if (Config::current_preset.has_value()) {
						if (key == "p") {
							if(++(*Config::current_preset) >= static_cast<int>(Config::preset_list.size())) Config::current_preset = first_preset;
						}
						else if (--(*Config::current_preset) < first_preset) Config::current_preset = Config::preset_list.size() - 1;
					}
					else Config::current_preset = (key == "p") ? first_preset : Config::preset_list.size() - 1;
					if (Config::current_preset == old_preset) return;
					if (not Config::apply_preset(Config::preset_list.at(Config::current_preset.value()))) {
						Menu::show(Menu::Menus::SizeError);
						Config::current_preset = old_preset;
						return;
					}
					Draw::calcSizes();
					Draw::update_clock(true);
					Runner::run("all", false, true);
					return;
				} else if (is_in(key, "ctrl_r")) {
#if defined(_WIN32)
					Global::reload_conf = true;
					interrupt();
#else
					kill(getpid(), SIGUSR2);
#endif
					return;
				} else if (key == "mouse_release") {
					dragging_scroll = false;
				} else
					keep_going = true;

				if (not keep_going) return;
			}

			//? Input actions for proc box
			if (Proc::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;
				if (filtering) {
					if (key == "enter" or key == "down") {
						Config::set("proc_filter", Proc::filter.text);
						Config::set("proc_filtering", false);
						old_filter.clear();
						if(key == "down"){
							Config::unlock();
							Config::lock();
							process("down");
							return;
						}
					}
					else if (key == "escape" or key == "mouse_click") {
						Config::set("proc_filter", old_filter);
						Config::set("proc_filtering", false);
						old_filter.clear();
					}
					else if (Proc::filter.command(key)) {
						if (Config::getS("proc_filter") != Proc::filter.text)
							Config::set("proc_filter", Proc::filter.text);
					}
					else
						return;
				}
				else if (key == "left" or (vim_keys and key == "h")) {
					int cur_i = v_index(Proc::sort_vector, Config::getS("proc_sorting"));
					if (--cur_i < 0)
						cur_i = Proc::sort_vector.size() - 1;
					Config::set("proc_sorting", Proc::sort_vector.at(cur_i));
					Config::set("update_following", true);
					if (Config::getB("proc_tree")) no_update = false;
				}
				else if (key == "right" or (vim_keys and key == "l")) {
					int cur_i = v_index(Proc::sort_vector, Config::getS("proc_sorting"));
					if (std::cmp_greater(++cur_i, Proc::sort_vector.size() - 1))
						cur_i = 0;
					Config::set("proc_sorting", Proc::sort_vector.at(cur_i));
					Config::set("update_following", true);
					if (Config::getB("proc_tree")) no_update = false;
				}
				else if (is_in(key, "f", "/")) {
					Config::flip("proc_filtering");
					Proc::filter = Draw::TextEdit{Config::getS("proc_filter")};
					old_filter = Proc::filter.text;
				}
				else if (key == "e") {
					Config::flip("proc_tree");
					no_update = false;
					Config::set("update_following", true);
				}
				else if (key == "E" and Config::getB("proc_tree")) {
					atomic_wait(Runner::active);
					Proc::collapse_all = 1;
					no_update = false;
				}
				else if (is_in(key, "u")) {
					Config::flip("pause_proc_list");
				}
				else if (is_in(key, "F")) {
					if (Config::getI("proc_selected") != 0 and Config::getI("followed_pid") != Config::getI("selected_pid")) {
						Config::set("follow_process", true);
						Config::set("followed_pid", Config::getI("selected_pid"));
						Config::set("update_following", true);
					}
					else if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Config::getI("followed_pid") != Config::getI("detailed_pid")) {
						Config::set("follow_process", true);
						Config::set("followed_pid", Config::getI("detailed_pid"));
						Config::set("update_following", true);
					}
					else if (Config::getB("follow_process")) {
						Config::flip("follow_process");
						if (Config::getB("should_selection_return_to_followed"))
							Config::set("proc_selected", Config::getI("proc_followed"));
						else if (Config::getB("show_detailed") and Config::getI("followed_pid") == Config::getI("detailed_pid"))
							Config::set("restore_detailed_pid", Config::getI("detailed_pid"));
						Config::set("followed_pid", 0);
						Config::set("proc_followed", 0);
					}
				}
				else if (key == "r") {
					Config::flip("proc_reversed");
					Config::set("update_following", true);
				}
				else if (key == "c")
					Config::flip("proc_per_core");

				else if (key == "%")
					Config::flip("proc_mem_bytes");

				else if (key == "delete" and not Config::getS("proc_filter").empty())
					Config::set("proc_filter", ""s);

				else if (key.starts_with("mouse_")) {
					redraw = false;
					const auto& [col, line] = mouse_pos;
					const int y = (Config::getB("show_detailed") ? Proc::y + 8 : Proc::y);
					const int height = (Config::getB("show_detailed") ? Proc::height - 8 : Proc::height);
					const auto in_proc_box = col >= Proc::x + 1 and col < Proc::x + Proc::width and line >= y + 1 and line < y + height - 1;
					if (key == "mouse_click") {
						if (in_proc_box) {
							if (col < Proc::x + Proc::width - 2) {
								const auto& current_selection = Config::getI("proc_selected");
								if (current_selection == line - y - 1) {
									redraw = true;
									if (Config::getB("proc_tree")) {
										const int x_pos = col - Proc::x;
										const int offset = Config::getI("selected_depth") * 3;
										if (x_pos > offset and x_pos < 4 + offset) {
											process("space");
											return;
										}
									}
									process("enter");
									return;
								}
								else if (Config::getB("proc_banner_shown") and line == y + height - 2)
									return;
								else if (current_selection == 0 or line - y - 1 == 0)
									redraw = true;

								if (Config::getB("follow_process") and not Config::getB("pause_proc_list")) {
									Config::flip("follow_process");
									Config::set("followed_pid", 0);
									Config::set("proc_followed", 0);
									redraw = true;
								}

								Config::set("proc_selected", line - y - 1);
							}
							else if (line == y + 1) {
								if (Proc::selection("page_up") == -1) return;
							}
							else if (line == y + height - 2) {
								if (Proc::selection("page_down") == -1) return;
							}
							else if (line == y + 2 + Proc::scroll_pos) {
								dragging_scroll = true;
							}
							else if (Proc::selection("mousey" + to_string(line - y - 2)) == -1)
								return;
						}
						else if (Config::getI("proc_selected") > 0){
							Config::set("proc_selected", 0);
							if (Config::getB("follow_process") and not Config::getB("pause_proc_list")) {
								Config::flip("follow_process");
								Config::set("followed_pid", 0);
								Config::set("proc_followed", 0);
							}
							redraw = true;
						}
					}
					else if (key.starts_with("mouse_scroll_") and in_proc_box) {
						goto proc_mouse_scroll;
					}
					else if (key == "mouse_drag" and dragging_scroll) {
						Proc::selection("mousey" + to_string(line - y - 2));
					}
					else
						keep_going = true;
				}
				else if (is_in(key, "enter", "info_enter")) {
					if (Config::getI("proc_selected") == 0 and not Config::getB("show_detailed")) {
						return;
					}
					else if (Config::getI("proc_selected") > 0 and Config::getI("detailed_pid") != Config::getI("selected_pid")) {
						Config::set("detailed_pid", Config::getI("selected_pid"));
						Config::set("proc_last_selected", Config::getI("proc_selected"));
						Config::set("proc_selected", 0);
						if (Config::getB("proc_follow_detailed")) {
							Config::set("follow_process", true);
							Config::set("followed_pid", Config::getI("selected_pid"));
						}
						Config::set("show_detailed", true);
					}
					else if (Config::getB("show_detailed")) {
						if (Config::getB("proc_follow_detailed")) {
							Config::set("restore_detailed_pid", Config::getI("detailed_pid"));
							if (Config::getB("follow_process") and Config::getI("followed_pid") == Config::getI("detailed_pid")) {
								Config::flip("follow_process");
								Config::set("followed_pid", 0);
								Config::set("proc_followed", 0);
							}
						}
						else if (Config::getI("proc_last_selected") > 0) Config::set("proc_selected", Config::getI("proc_last_selected"));
						Config::set("proc_last_selected", 0);
						Config::set("detailed_pid", 0);
						Config::set("show_detailed", false);
					}
					Config::set("update_following", true);
				}
				else if (is_in(key, "+", "-", "space", "C", "=") and Config::getB("proc_tree")) {
					const bool is_following_detailed = Config::getB("follow_process") and Config::getI("followed_pid") == Config::getI("detailed_pid");
					if (Config::getI("proc_selected") > 0 or is_following_detailed) {
						atomic_wait(Runner::active);
						auto& pid = is_following_detailed and Config::getI("proc_selected") == 0 ? Config::getI("followed_pid") : Config::getI("selected_pid");
						if (key == "+" or key == "space" or key == "=") Proc::expand = pid;
						if (key == "-" or key == "space") Proc::collapse = pid;
						if (key == "C")	Proc::toggle_children = pid;
						no_update = false;
					}
					else
						keep_going = true;
				}
				else if (is_in(key, "t", kill_key) and (Config::getB("show_detailed") or Config::getI("selected_pid") > 0)) {
					atomic_wait(Runner::active);
					if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Proc::detailed.status == "Dead") return;
					Menu::show(Menu::Menus::SignalSend, (key == "t" ? SIGTERM : SIGKILL));
					return;
				}
				else if (key == "s" and (Config::getB("show_detailed") or Config::getI("selected_pid") > 0)) {
					atomic_wait(Runner::active);
					if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Proc::detailed.status == "Dead") return;
					Menu::show(Menu::Menus::SignalChoose);
					return;
				}
				else if (key == "N" and (Config::getB("show_detailed") or Config::getI("selected_pid") > 0)) {
					atomic_wait(Runner::active);
				    if (Config::getB("show_detailed") and Config::getI("proc_selected") == 0 and Proc::detailed.status == "Dead") return;
				    Menu::show(Menu::Menus::Renice);
				    return;
			    }
				else if (is_in(key, "up", "down", "page_up", "page_down", "home", "end") or (vim_keys and is_in(key, "j", "k", "g", "G"))) {
					proc_mouse_scroll:
					redraw = false;
					auto old_selected = Config::getI("proc_selected");
					auto new_selected = Proc::selection(key);
					if (new_selected == -1)
						return;
					else if (old_selected != new_selected and (old_selected == 0 or new_selected == 0))
						redraw = true;
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("proc", no_update, redraw);
					Runner::run("cpu", no_update, redraw);
					return;
				}
			}

			//? Input actions for cpu box
			if (Cpu::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;
				static uint64_t last_press = 0;

				if ((key == "+" or key == "=") and Config::getI("update_ms") <= 86399900) {
					int add = (Config::getI("update_ms") <= 86399000 and last_press >= time_ms() - 200
						and rng::all_of(Input::history, [](const auto& str){ return str == "+"; })
						? 1000 : 100);
					Config::set("update_ms", Config::getI("update_ms") + add);
					last_press = time_ms();
					redraw = true;
				}
				else if (key == "-" and Config::getI("update_ms") >= 200) {
					int sub = (Config::getI("update_ms") >= 2000 and last_press >= time_ms() - 200
						and rng::all_of(Input::history, [](const auto& str){ return str == "-"; })
						? 1000 : 100);
					Config::set("update_ms", Config::getI("update_ms") - sub);
					last_press = time_ms();
					redraw = true;
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("cpu", no_update, redraw);
					return;
				}
			}

			//? Input actions for mem box
			if (Mem::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;

				if (key == "i") {
					Config::flip("io_mode");
				}
				else if (key == "d") {
					Config::flip("show_disks");
					no_update = false;
					Draw::calcSizes();
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("mem", no_update, redraw);
					return;
				}
			}

			//? Input actions for net box
			if (Net::shown) {
				bool keep_going = false;
				bool no_update = true;
				bool redraw = true;

				if (is_in(key, "b", "n")) {
					atomic_wait(Runner::active);
					int c_index = v_index(Net::interfaces, Net::selected_iface);
					if (c_index != (int)Net::interfaces.size()) {
						if (key == "b") {
							if (--c_index < 0) c_index = Net::interfaces.size() - 1;
						}
						else if (key == "n") {
							if (++c_index == (int)Net::interfaces.size()) c_index = 0;
						}
						Net::selected_iface = Net::interfaces.at(c_index);
						Config::set("net_iface", Net::selected_iface);
						Net::rescale = true;
					}
				}
				else if (key == "y") {
					Config::flip("net_sync");
					Net::rescale = true;
				}
				else if (key == "a") {
					Config::flip("net_auto");
					Net::rescale = true;
				}
				else if (key == "z") {
					atomic_wait(Runner::active);
					if (Net::current_net.contains(Net::selected_iface)) {
						auto& ndev = Net::current_net.at(Net::selected_iface);
						if (ndev.stat.at("download").offset + ndev.stat.at("upload").offset > 0) {
							ndev.stat.at("download").offset = 0;
							ndev.stat.at("upload").offset = 0;
						}
						else {
							ndev.stat.at("download").offset = ndev.stat.at("download").last + ndev.stat.at("download").rollover;
							ndev.stat.at("upload").offset = ndev.stat.at("upload").last + ndev.stat.at("upload").rollover;
						}
						no_update = false;
					}
				}
				else keep_going = true;

				if (not keep_going) {
					Runner::run("net", no_update, redraw);
					return;
				}
			}
		}

		catch (const std::exception& e) {
			throw std::runtime_error { fmt::format(R"(Input::process("{}"))", e.what()) };
		}
	}
}
