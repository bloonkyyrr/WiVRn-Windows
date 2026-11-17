#pragma once

#include "wivrn_packets.h"

#include <cstdint>
#include <vector>

class wivrn_uinput
{
public:
	// Devices are created lazily on first use, so only forwarded classes are exposed.
	wivrn_uinput() = default;

	void handle_input(wivrn::from_headset::hid::input &);

	// The gamepad device is created on the first packet with gamepad inputs
	void handle_gamepad(const wivrn::from_headset::inputs &);
	// Remove the virtual gamepad, when the headset stops mirroring or the gamepad disconnects
	void destroy_gamepad();

	// Drains pending force-feedback requests from the virtual gamepad and returns the
	// resulting haptics packets to forward to the headset, if any changed.
	std::vector<wivrn::to_headset::haptics> read_rumble();

private:
	void send_key(uint16_t key, bool down);
	void send_button(uint16_t mouse_button, bool down);
	void mouse_move_relative(int16_t x, int16_t y);
	void mouse_scroll(int16_t vertical, int16_t horizontal);

	bool gamepad_warning_logged = false;
};