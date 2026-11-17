#include "wivrn_uinput.h"

#include "util/u_logging.h"
#include "utils/overloaded.h"

#include <windows.h>

#include <cmath>
#include <system_error>

namespace
{
/*
 * The headset forwards Linux evdev key codes. Translate the ones that fit in
 * the packet's uint8_t field to Windows virtual-key codes; unmapped keys are
 * ignored rather than injected as the wrong key.
 */
uint16_t vk_from_linux_key(uint8_t key)
{
	// KEY_1..KEY_9 then KEY_0
	if (key >= 2 and key <= 10)
		return '1' + (key - 2);
	if (key == 11)
		return '0';

	// KEY_Q..KEY_P
	if (key >= 16 and key <= 25)
		return 'Q' + (key - 16);
	// KEY_A..KEY_L
	if (key >= 30 and key <= 38)
		return 'A' + (key - 30);
	// KEY_Z..KEY_M
	if (key >= 44 and key <= 50)
		return 'Z' + (key - 44);
	// KEY_F1..KEY_F10
	if (key >= 59 and key <= 68)
		return VK_F1 + (key - 59);

	switch (key)
	{
		case 1:
			return VK_ESCAPE; // KEY_ESC
		case 12:
			return VK_OEM_MINUS; // KEY_MINUS
		case 13:
			return VK_OEM_PLUS; // KEY_EQUAL
		case 14:
			return VK_BACK; // KEY_BACKSPACE
		case 15:
			return VK_TAB; // KEY_TAB
		case 26:
			return VK_OEM_4; // KEY_LEFTBRACE
		case 27:
			return VK_OEM_6; // KEY_RIGHTBRACE
		case 28:
			return VK_RETURN; // KEY_ENTER
		case 29:
			return VK_LCONTROL; // KEY_LEFTCTRL
		case 39:
			return VK_OEM_1; // KEY_SEMICOLON
		case 40:
			return VK_OEM_7; // KEY_APOSTROPHE
		case 41:
			return VK_OEM_3; // KEY_GRAVE
		case 42:
			return VK_LSHIFT; // KEY_LEFTSHIFT
		case 43:
			return VK_OEM_5; // KEY_BACKSLASH
		case 51:
			return VK_OEM_COMMA; // KEY_COMMA
		case 52:
			return VK_OEM_PERIOD; // KEY_DOT
		case 53:
			return VK_OEM_2; // KEY_SLASH
		case 54:
			return VK_RSHIFT; // KEY_RIGHTSHIFT
		case 55:
			return VK_MULTIPLY; // KEY_KPASTERISK
		case 56:
			return VK_LMENU; // KEY_LEFTALT
		case 57:
			return VK_SPACE; // KEY_SPACE
		case 58:
			return VK_CAPITAL; // KEY_CAPSLOCK
		case 69:
			return VK_NUMLOCK; // KEY_NUMLOCK
		case 70:
			return VK_SCROLL; // KEY_SCROLLLOCK
		case 71:
			return VK_NUMPAD7; // KEY_KP7
		case 72:
			return VK_NUMPAD8; // KEY_KP8
		case 73:
			return VK_NUMPAD9; // KEY_KP9
		case 74:
			return VK_SUBTRACT; // KEY_KPMINUS
		case 75:
			return VK_NUMPAD4; // KEY_KP4
		case 76:
			return VK_NUMPAD5; // KEY_KP5
		case 77:
			return VK_NUMPAD6; // KEY_KP6
		case 78:
			return VK_ADD; // KEY_KPPLUS
		case 79:
			return VK_NUMPAD1; // KEY_KP1
		case 80:
			return VK_NUMPAD2; // KEY_KP2
		case 81:
			return VK_NUMPAD3; // KEY_KP3
		case 82:
			return VK_NUMPAD0; // KEY_KP0
		case 83:
			return VK_DECIMAL; // KEY_KPDOT
		case 86:
			return VK_OEM_102; // KEY_102ND
		case 87:
			return VK_F11; // KEY_F11
		case 88:
			return VK_F12; // KEY_F12
		case 96:
			return VK_RETURN; // KEY_KPENTER
		case 97:
			return VK_RCONTROL; // KEY_RIGHTCTRL
		case 98:
			return VK_DIVIDE; // KEY_KPSLASH
		case 99:
			return VK_SNAPSHOT; // KEY_SYSRQ
		case 100:
			return VK_RMENU; // KEY_RIGHTALT
		case 102:
			return VK_HOME; // KEY_HOME
		case 103:
			return VK_UP; // KEY_UP
		case 104:
			return VK_PRIOR; // KEY_PAGEUP
		case 105:
			return VK_LEFT; // KEY_LEFT
		case 106:
			return VK_RIGHT; // KEY_RIGHT
		case 107:
			return VK_END; // KEY_END
		case 108:
			return VK_DOWN; // KEY_DOWN
		case 109:
			return VK_NEXT; // KEY_PAGEDOWN
		case 110:
			return VK_INSERT; // KEY_INSERT
		case 111:
			return VK_DELETE; // KEY_DELETE
		case 113:
			return VK_VOLUME_MUTE; // KEY_MUTE
		case 114:
			return VK_VOLUME_DOWN; // KEY_VOLUMEDOWN
		case 115:
			return VK_VOLUME_UP; // KEY_VOLUMEUP
		case 117:
			return VK_OEM_NEC_EQUAL; // KEY_KPEQUAL
		case 119:
			return VK_PAUSE; // KEY_PAUSE
		case 125:
			return VK_LWIN; // KEY_LEFTMETA
		case 126:
			return VK_RWIN; // KEY_RIGHTMETA
		case 127:
			return VK_APPS; // KEY_COMPOSE
		case 142:
			return VK_SLEEP; // KEY_SLEEP
		case 155:
			return VK_LAUNCH_MAIL; // KEY_MAIL
		case 156:
			return VK_BROWSER_FAVORITES; // KEY_BOOKMARKS
		case 158:
			return VK_BROWSER_BACK; // KEY_BACK
		case 159:
			return VK_BROWSER_FORWARD; // KEY_FORWARD
		case 163:
			return VK_MEDIA_NEXT_TRACK; // KEY_NEXTSONG
		case 164:
			return VK_MEDIA_PLAY_PAUSE; // KEY_PLAYPAUSE
		case 165:
			return VK_MEDIA_PREV_TRACK; // KEY_PREVIOUSSONG
		case 166:
			return VK_MEDIA_STOP; // KEY_STOPCD
		case 172:
			return VK_BROWSER_HOME; // KEY_HOMEPAGE
		case 173:
			return VK_BROWSER_REFRESH; // KEY_REFRESH
		default:
			return 0;
	}
}

bool is_extended_key(uint16_t vk)
{
	switch (vk)
	{
		case VK_INSERT:
		case VK_DELETE:
		case VK_HOME:
		case VK_END:
		case VK_PRIOR:
		case VK_NEXT:
		case VK_LEFT:
		case VK_RIGHT:
		case VK_UP:
		case VK_DOWN:
		case VK_DIVIDE:
		case VK_RCONTROL:
		case VK_RMENU:
		case VK_NUMLOCK:
		case VK_SNAPSHOT:
		case VK_LWIN:
		case VK_RWIN:
		case VK_APPS:
		case VK_SLEEP:
		case VK_BROWSER_BACK:
		case VK_BROWSER_FORWARD:
		case VK_BROWSER_REFRESH:
		case VK_BROWSER_STOP:
		case VK_BROWSER_SEARCH:
		case VK_BROWSER_FAVORITES:
		case VK_BROWSER_HOME:
		case VK_VOLUME_MUTE:
		case VK_VOLUME_DOWN:
		case VK_VOLUME_UP:
		case VK_MEDIA_NEXT_TRACK:
		case VK_MEDIA_PREV_TRACK:
		case VK_MEDIA_STOP:
		case VK_MEDIA_PLAY_PAUSE:
		case VK_LAUNCH_MAIL:
		case VK_LAUNCH_APP2:
			return true;
		default:
			return false;
	}
}

void send_input_or_throw(INPUT & input)
{
	if (SendInput(1, &input, sizeof(INPUT)) != 1)
		throw std::system_error((int)GetLastError(), std::system_category(), "SendInput");
}
} // namespace

void wivrn_uinput::handle_input(wivrn::from_headset::hid::input & e)
{
	std::visit(utils::overloaded{
	                   [this](const wivrn::from_headset::hid::key_down & e) {
		                   send_key(e.key, true);
	                   },
	                   [this](const wivrn::from_headset::hid::key_up & e) {
		                   send_key(e.key, false);
	                   },
	                   [this](const wivrn::from_headset::hid::button_down & e) {
		                   send_button(e.button, true);
	                   },
	                   [this](const wivrn::from_headset::hid::button_up & e) {
		                   send_button(e.button, false);
	                   },
	                   [this](const wivrn::from_headset::hid::mouse_scroll & e) {
		                   mouse_scroll((int16_t)std::lround(e.v), (int16_t)std::lround(e.h));
	                   },
	                   [this](const wivrn::from_headset::hid::mouse_move & e) {
		                   mouse_move_relative((int16_t)std::lround(e.x), (int16_t)std::lround(e.y));
	                   },
	           },
	           e.input_data);
}

void wivrn_uinput::handle_gamepad(const wivrn::from_headset::inputs &)
{
	// Windows has no user-mode virtual gamepad. Mirroring to non-OpenXR apps
	// needs a kernel driver such as ViGEmBus; the OpenXR gamepad is unaffected.
	if (not gamepad_warning_logged)
	{
		U_LOG_I("Gamepad mirroring requires a virtual gamepad driver (for example ViGEmBus); skipping");
		gamepad_warning_logged = true;
	}
}

void wivrn_uinput::destroy_gamepad() {}

std::vector<wivrn::to_headset::haptics> wivrn_uinput::read_rumble()
{
	return {};
}

void wivrn_uinput::send_key(uint16_t key, bool down)
{
	uint16_t vk = vk_from_linux_key((uint8_t)key);
	if (vk == 0)
		return;

	INPUT input{};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk;
	// Numpad Enter shares VK_RETURN with the main Enter key and needs the
	// extended flag to stay distinct.
	const bool extended = is_extended_key(vk) or key == 96;
	input.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
	send_input_or_throw(input);
}

/// 0: left, 1: right, 2: middle
void wivrn_uinput::send_button(uint16_t mouse_button, bool down)
{
	DWORD flag = 0;
	switch (mouse_button)
	{
		case 0:
			flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
			break;
		case 1:
			flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
			break;
		case 2:
			flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
			break;
		default:
			return;
	}

	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dwFlags = flag;
	send_input_or_throw(input);
}

void wivrn_uinput::mouse_move_relative(int16_t x, int16_t y)
{
	if (x == 0 and y == 0)
		return;

	INPUT input{};
	input.type = INPUT_MOUSE;
	input.mi.dx = x;
	input.mi.dy = y;
	input.mi.dwFlags = MOUSEEVENTF_MOVE;
	send_input_or_throw(input);
}

void wivrn_uinput::mouse_scroll(int16_t vertical, int16_t horizontal)
{
	if (vertical != 0)
	{
		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.mouseData = (DWORD)(int)vertical * WHEEL_DELTA;
		input.mi.dwFlags = MOUSEEVENTF_WHEEL;
		send_input_or_throw(input);
	}

	if (horizontal != 0)
	{
		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.mouseData = (DWORD)(int)horizontal * WHEEL_DELTA;
		input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
		send_input_or_throw(input);
	}
}

