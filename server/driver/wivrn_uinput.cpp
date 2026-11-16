#include "wivrn_uinput.h"

#if defined(_WIN32)

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

#else

#include "utils/overloaded.h"
#include "wivrn_gamepad_map.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <system_error>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <linux/uinput.h>

namespace
{
wivrn::fd_base open_uinput_or_throw(int flags = O_WRONLY)
{
	constexpr std::array paths = {"/dev/uinput", "/dev/input/uinput"};
	for (const char * p: paths)
	{
		int fd = ::open(p, flags | O_NONBLOCK);
		if (fd >= 0)
			return fd;
		if (errno != ENOENT) // exists but inaccessible
			throw std::system_error(errno, std::generic_category(), "error while opening uinput");
	}
	throw std::runtime_error("no uinput device found");
}

void write_or_throw(int fd, const void * buf, size_t n)
{
	auto res = ::write(fd, buf, n);
	if (res < 0)
		throw std::system_error(errno, std::generic_category(), "error during write");
	if (res != n)
		throw std::runtime_error("byte count mismatch while writing to uinput");
}

void fill_ids(uinput_user_dev & uidev, uint16_t vendor, uint16_t product) noexcept
{
	std::memset(&uidev, 0, sizeof(uidev));
	uidev.id.bustype = 0x03;
	uidev.id.vendor = vendor;
	uidev.id.product = product;
	uidev.id.version = 5;
}

void fill_ids(uinput_user_dev & uidev, uint16_t product) noexcept
{
	fill_ids(uidev, 0x4711, product);
}

void emit_ev(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev;
	std::memset(&ev, 0, sizeof(ev));
	gettimeofday(&ev.time, nullptr);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	write_or_throw(fd, &ev, sizeof(ev));
}

void syn(int fd)
{
	emit_ev(fd, EV_SYN, SYN_REPORT, 0);
}

void ioctl_or_throw(int fd, unsigned long op)
{
	auto n = ioctl(fd, op);
	if (n < 0)
		throw std::system_error(errno, std::generic_category(), "ioctl error");
}
void ioctl_or_throw(int fd, unsigned long op, unsigned long int arg)
{
	auto n = ioctl(fd, op, arg);
	if (n < 0)
		throw std::system_error(errno, std::generic_category(), "ioctl error");
}

} // namespace

void wivrn_uinput::handle_input(wivrn::from_headset::hid::input & e)
{
	if (kbd_fd.get_fd() < 0)
	{
		init_keyboard();
		init_mouse();
	}

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

namespace
{
int16_t scale_stick(float v)
{
	v = std::clamp(v, -1.f, 1.f);
	return (int16_t)std::lround(v * 32767.f);
}

int16_t scale_trigger(float v)
{
	v = std::clamp(v, 0.f, 1.f);
	return (int16_t)std::lround(v * 255.f);
}
} // namespace

void wivrn_uinput::handle_gamepad(const wivrn::from_headset::inputs & inputs)
{
	using namespace wivrn;
	if (gamepad_fd.get_fd() < 0)
	{
		// Inputs packets are mostly controller inputs: don't create the device
		// until the headset actually sends gamepad inputs
		if (std::ranges::none_of(inputs.values, [](const auto & value) { return map_gamepad(value.id).slot != gp_count; }))
			return;
		init_gamepad();
	}
	int fd = gamepad_fd.get_fd();

	bool dpad_left = false, dpad_right = false, dpad_up = false, dpad_down = false;

	for (const auto & value: inputs.values)
	{
		auto m = map_gamepad(value.id);
		if (m.slot == gp_count)
			continue;
		switch (m.kind)
		{
			case gp_value::button:
				emit_ev(fd, EV_KEY, m.ev_code, value.value != 0);
				break;
			case gp_value::trigger:
				emit_ev(fd, EV_ABS, m.ev_code, scale_trigger(value.value));
				break;
			case gp_value::stick_x:
			case gp_value::stick_y:
				emit_ev(fd, EV_ABS, m.ev_code, scale_stick(value.value));
				break;
			case gp_value::dpad:
				switch (m.slot)
				{
					case gp_dpad_left:
						dpad_left = value.value != 0;
						break;
					case gp_dpad_right:
						dpad_right = value.value != 0;
						break;
					case gp_dpad_up:
						dpad_up = value.value != 0;
						break;
					case gp_dpad_down:
						dpad_down = value.value != 0;
						break;
					default:
						break;
				}
				break;
		}
	}

	emit_ev(fd, EV_ABS, ABS_HAT0X, (dpad_right ? 1 : 0) - (dpad_left ? 1 : 0));
	emit_ev(fd, EV_ABS, ABS_HAT0Y, (dpad_down ? 1 : 0) - (dpad_up ? 1 : 0));
	syn(fd);
}

void wivrn_uinput::destroy_gamepad()
{
	if (gamepad_fd.get_fd() < 0)
		return;
	ioctl(gamepad_fd.get_fd(), UI_DEV_DESTROY);
	gamepad_fd = {};
	ff_effects.clear();
}

std::vector<wivrn::to_headset::haptics> wivrn_uinput::read_rumble()
{
	int fd = gamepad_fd.get_fd();
	if (fd < 0)
		return {};

	std::vector<wivrn::to_headset::haptics> result;
	auto motors = [&](float strong, float weak, uint16_t length_ms) {
		// 0 length is a continuous effect; send a finite burst instead
		const std::chrono::nanoseconds duration{int64_t(length_ms ? length_ms : 100) * 1'000'000};
		// strong/weak are motor weights, not locations, so drive both outputs at the combined intensity
		const float amplitude = std::max(strong, weak);
		result = {
		        {wivrn::device_id::GAMEPAD_HAPTIC_LEFT, duration, 0.f, amplitude},
		        {wivrn::device_id::GAMEPAD_HAPTIC_RIGHT, duration, 0.f, amplitude},
		};
	};

	input_event ev;
	while (::read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
	{
		if (ev.type == EV_UINPUT and ev.code == UI_FF_UPLOAD)
		{
			uinput_ff_upload upload{
			        .request_id = (uint32_t)ev.value,
			};
			if (ioctl(fd, UI_BEGIN_FF_UPLOAD, &upload) == 0)
			{
				ff_effects[upload.effect.id] = upload.effect;
				upload.retval = 0;
				ioctl(fd, UI_END_FF_UPLOAD, &upload);
			}
		}
		else if (ev.type == EV_UINPUT and ev.code == UI_FF_ERASE)
		{
			uinput_ff_erase erase{
			        .request_id = (uint32_t)ev.value,
			};
			if (ioctl(fd, UI_BEGIN_FF_ERASE, &erase) == 0)
			{
				ff_effects.erase(erase.effect_id);
				erase.retval = 0;
				ioctl(fd, UI_END_FF_ERASE, &erase);
			}
		}
		else if (ev.type == EV_FF)
		{
			// ev.code is the effect id, ev.value != 0 starts it, 0 stops it.
			if (ev.value == 0)
			{
				motors(0.f, 0.f, 0);
			}
			else if (auto it = ff_effects.find(ev.code); it != ff_effects.end() and it->second.type == FF_RUMBLE)
			{
				const auto & rumble = it->second.u.rumble;
				motors(rumble.strong_magnitude / 65535.f, rumble.weak_magnitude / 65535.f, (uint16_t)it->second.replay.length);
			}
		}
	}

	return result;
}

void wivrn_uinput::send_key(uint16_t key, bool down)
{
	emit_ev(kbd_fd, EV_KEY, key, down ? 1 : 0);
	syn(kbd_fd);
}

/// 0: left, 1: right, 2: middle
void wivrn_uinput::send_button(uint16_t mouse_button, bool down)
{
	mouse_button += BTN_MOUSE;
	emit_ev(mouse_fd, EV_KEY, mouse_button, down ? 1 : 0);
	syn(mouse_fd);
}

void wivrn_uinput::mouse_move_relative(int16_t x, int16_t y)
{
	if (x)
		emit_ev(mouse_fd, EV_REL, REL_X, x);
	if (y)
		emit_ev(mouse_fd, EV_REL, REL_Y, y);
	syn(mouse_fd);
}

void wivrn_uinput::mouse_scroll(int16_t vertical, int16_t horizontal)
{
	if (vertical)
		emit_ev(mouse_fd, EV_REL, REL_WHEEL, vertical);
	if (horizontal)
		emit_ev(mouse_fd, EV_REL, REL_HWHEEL, horizontal);
	syn(mouse_fd);
}

void wivrn_uinput::init_keyboard()
{
	kbd_fd = open_uinput_or_throw();
	int fd = kbd_fd.get_fd();

	ioctl_or_throw(fd, UI_SET_EVBIT, EV_KEY);
	ioctl_or_throw(fd, UI_SET_EVBIT, EV_REP);

	// enable all KEY_* to allow arbitrary key codes
	for (int code = 1; code <= KEY_MAX; ++code)
		ioctl_or_throw(fd, UI_SET_KEYBIT, code);

	uinput_user_dev uidev;
	fill_ids(uidev, 0x0840);
	std::snprintf(uidev.name, sizeof(uidev.name), "WiVRn Keyboard");
	write_or_throw(fd, &uidev, sizeof(uidev));
	ioctl_or_throw(fd, UI_DEV_CREATE);
}

void wivrn_uinput::init_mouse()
{
	mouse_fd = open_uinput_or_throw();
	int fd = mouse_fd.get_fd();

	ioctl_or_throw(fd, UI_SET_EVBIT, EV_KEY);
	ioctl_or_throw(fd, UI_SET_EVBIT, EV_REL);

	// buttons
	ioctl_or_throw(fd, UI_SET_KEYBIT, BTN_LEFT);
	ioctl_or_throw(fd, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl_or_throw(fd, UI_SET_KEYBIT, BTN_MIDDLE);

	// relative motion + wheels
	ioctl_or_throw(fd, UI_SET_RELBIT, REL_X);
	ioctl_or_throw(fd, UI_SET_RELBIT, REL_Y);
	ioctl_or_throw(fd, UI_SET_RELBIT, REL_WHEEL);
	ioctl_or_throw(fd, UI_SET_RELBIT, REL_HWHEEL);

	uinput_user_dev uidev;
	fill_ids(uidev, 0x0839);
	std::snprintf(uidev.name, sizeof(uidev.name), "WiVRn Mouse");
	write_or_throw(fd, &uidev, sizeof(uidev));
	ioctl_or_throw(fd, UI_DEV_CREATE);
}

void wivrn_uinput::init_gamepad()
{
	// O_RDWR to read force-feedback (rumble) back from the device
	gamepad_fd = open_uinput_or_throw(O_RDWR);
	int fd = gamepad_fd.get_fd();

	ioctl_or_throw(fd, UI_SET_EVBIT, EV_KEY);
	ioctl_or_throw(fd, UI_SET_EVBIT, EV_ABS);
	ioctl_or_throw(fd, UI_SET_EVBIT, EV_FF);
	ioctl_or_throw(fd, UI_SET_FFBIT, FF_RUMBLE);

	// BTN_GAMEPAD (== BTN_SOUTH) is required or SDL ignores the device
	for (int code: {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR})
		ioctl_or_throw(fd, UI_SET_KEYBIT, code);

	for (int code: {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y})
		ioctl_or_throw(fd, UI_SET_ABSBIT, code);

	uinput_user_dev uidev;
	// Microsoft X-Box vendor/product so Steam Input also recognizes it by GUID
	fill_ids(uidev, 0x045e, 0x028e);
	uidev.ff_effects_max = ff_effects_max;
	std::snprintf(uidev.name, sizeof(uidev.name), "WiVRn Gamepad");

	const auto set_abs = [&](int code, int32_t min, int32_t max, int32_t fuzz, int32_t flat) {
		uidev.absmin[code] = min;
		uidev.absmax[code] = max;
		uidev.absfuzz[code] = fuzz;
		uidev.absflat[code] = flat;
	};
	for (int code: {ABS_X, ABS_Y, ABS_RX, ABS_RY})
		set_abs(code, -32768, 32767, 16, 128);
	set_abs(ABS_Z, 0, 255, 0, 0);
	set_abs(ABS_RZ, 0, 255, 0, 0);
	set_abs(ABS_HAT0X, -1, 1, 0, 0);
	set_abs(ABS_HAT0Y, -1, 1, 0, 0);

	write_or_throw(fd, &uidev, sizeof(uidev));
	ioctl_or_throw(fd, UI_DEV_CREATE);
}

#endif