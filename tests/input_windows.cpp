// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#if defined(_WIN32)
#include "btop_input.hpp"

TEST(input_windows, translates_navigation_and_function_keys) {
	EXPECT_EQ(Input::windows_key_to_input(VK_UP, 0, 0), "\033[A");
	EXPECT_EQ(Input::windows_key_to_input(VK_DOWN, 0, 0), "\033[B");
	EXPECT_EQ(Input::windows_key_to_input(VK_LEFT, 0, 0), "\033[D");
	EXPECT_EQ(Input::windows_key_to_input(VK_RIGHT, 0, 0), "\033[C");
	EXPECT_EQ(Input::windows_key_to_input(VK_PRIOR, 0, 0), "\033[5~");
	EXPECT_EQ(Input::windows_key_to_input(VK_NEXT, 0, 0), "\033[6~");
	EXPECT_EQ(Input::windows_key_to_input(VK_HOME, 0, 0), "\033[H");
	EXPECT_EQ(Input::windows_key_to_input(VK_END, 0, 0), "\033[F");
	EXPECT_EQ(Input::windows_key_to_input(VK_F1, 0, 0), "\033OP");
	EXPECT_EQ(Input::windows_key_to_input(VK_F2, 0, 0), "\033OQ");
	EXPECT_EQ(Input::windows_key_to_input(VK_F12, 0, 0), "\033[24~");
}

TEST(input_windows, translates_control_and_text_keys) {
	EXPECT_EQ(Input::windows_key_to_input(VK_ESCAPE, 0, 0), "\033");
	EXPECT_EQ(Input::windows_key_to_input(VK_RETURN, 0, 0), "\n");
	EXPECT_EQ(Input::windows_key_to_input(VK_BACK, 0, 0), "\x7f");
	EXPECT_EQ(Input::windows_key_to_input(VK_TAB, 0, 0), "\t");
	EXPECT_EQ(Input::windows_key_to_input(VK_TAB, 0, SHIFT_PRESSED), "\033[Z");
	EXPECT_EQ(Input::windows_key_to_input(0, 0x03, 0), "\x03");
	EXPECT_EQ(Input::windows_key_to_input(0, 0x12, 0), "\x12");
	EXPECT_EQ(Input::windows_key_to_input(0, L'a', 0), "a");
	EXPECT_EQ(Input::windows_key_to_input(0, L'a', 0, false), "");
}

TEST(input_windows, translates_utf16_surrogate_pair_text) {
	EXPECT_EQ(Input::windows_key_to_input(0, static_cast<wchar_t>(0xD83D), 0), "");
	EXPECT_EQ(Input::windows_key_to_input(0, static_cast<wchar_t>(0xDE00), 0), "\xF0\x9F\x98\x80");
	EXPECT_EQ(Input::windows_key_to_input(0, static_cast<wchar_t>(0xD83D), 0), "");
	EXPECT_EQ(Input::windows_key_to_input(0, L'x', 0), "x");
	EXPECT_EQ(Input::windows_key_to_input(0, static_cast<wchar_t>(0xDE00), 0), "");
}

TEST(input_windows, translates_mouse_events_to_sgr_sequences) {
	EXPECT_EQ(Input::windows_mouse_to_input(0, FROM_LEFT_1ST_BUTTON_PRESSED, 4, 9), "\033[<0;5;10M");
	EXPECT_EQ(Input::windows_mouse_to_input(DOUBLE_CLICK, FROM_LEFT_1ST_BUTTON_PRESSED, 4, 9), "\033[<0;5;10M");
	EXPECT_EQ(Input::windows_mouse_to_input(0, 0, 4, 9), "\033[<0;5;10m");
	EXPECT_EQ(Input::windows_mouse_to_input(0, RIGHTMOST_BUTTON_PRESSED, 4, 9), "");
	EXPECT_EQ(Input::windows_mouse_to_input(MOUSE_MOVED, FROM_LEFT_1ST_BUTTON_PRESSED, 4, 9), "\033[<32;5;10M");
	EXPECT_EQ(Input::windows_mouse_to_input(MOUSE_MOVED, 0, 4, 9), "");
	EXPECT_EQ(Input::windows_mouse_to_input(MOUSE_WHEELED, static_cast<DWORD>(120) << 16, 4, 9), "\033[<64;5;10M");
	EXPECT_EQ(Input::windows_mouse_to_input(MOUSE_WHEELED, static_cast<DWORD>(static_cast<WORD>(-120)) << 16, 4, 9), "\033[<65;5;10M");
}
TEST(input_windows, normalizes_raw_key_sequences) {
	EXPECT_EQ(Input::normalize_raw_input("\033[A", false), "up");
	EXPECT_EQ(Input::normalize_raw_input("\033[6~", false), "page_down");
	EXPECT_EQ(Input::normalize_raw_input("\033[Z", false), "shift_tab");
	EXPECT_EQ(Input::normalize_raw_input("\033OQ", false), "f2");
	EXPECT_EQ(Input::normalize_raw_input("\x03", false), "q");
	EXPECT_EQ(Input::normalize_raw_input("\x12", false), "ctrl_r");
	EXPECT_EQ(Input::normalize_raw_input("a", false), "a");
	EXPECT_EQ(Input::normalize_raw_input("ab", false), "");
}

TEST(input_windows, normalizes_raw_mouse_sequences_and_coordinates) {
	EXPECT_EQ(Input::normalize_raw_input("\033[<0;5;10M", false), "mouse_click");
	EXPECT_EQ(Input::mouse_pos[0], 5);
	EXPECT_EQ(Input::mouse_pos[1], 10);
	EXPECT_EQ(Input::normalize_raw_input("\033[<32;7;12M", false), "mouse_drag");
	EXPECT_EQ(Input::mouse_pos[0], 7);
	EXPECT_EQ(Input::mouse_pos[1], 12);
	EXPECT_EQ(Input::normalize_raw_input("\033[<0;7;12m", false), "mouse_release");
	EXPECT_EQ(Input::normalize_raw_input("\033[<64;7;12M", false), "mouse_scroll_up");
	EXPECT_EQ(Input::normalize_raw_input("\033[<65;7;12M", false), "mouse_scroll_down");
	EXPECT_EQ(Input::normalize_raw_input("\033[<99;7;12M", false), "");
}
#endif