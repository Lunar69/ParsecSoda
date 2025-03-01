#include "GamepadClient.h"

// =============================================================
//
//  GAMEPAD ENGINE
// 
// =============================================================
GamepadClient::~GamepadClient()
{
	release();
}

void GamepadClient::setParsec(ParsecDSO* parsec)
{
	this->_parsec = parsec;
}

bool GamepadClient::init()
{
	_keyboardMap = KeyboardMap::defaultMap();

	if (_client != nullptr)
	{
		release();
	}

	_client = vigem_alloc();
	if (_client == nullptr)
	{
		return false;
	}

	const VIGEM_ERROR err = vigem_connect(_client);
	if (!VIGEM_SUCCESS(err))
	{
		return false;
	}

	return true;
}

Gamepad GamepadClient::createGamepad(uint16_t index)
{
	if (_client == nullptr)
	{
		return Gamepad();
	}

	Gamepad gamepad(_parsec, _client);
	gamepads.push_back(gamepad);
	return gamepad;
}

void GamepadClient::createMaximumGamepads()
{
	for (uint16_t i = 0; i < 8; i++)
	{
		this->createGamepad(i);
		Sleep(200);
	}
}

void GamepadClient::connectAllGamepads()
{
	reduce([](Gamepad& pad) {
		pad.connect();
		Sleep(200);
	});
}

void GamepadClient::disconnectAllGamepads()
{
	reduce([](Gamepad& pad) {
		pad.disconnect();
	});
}

void GamepadClient::sortGamepads()
{
	std::vector<Gamepad> sorted = gamepads;
	std::sort(
		sorted.begin(),
		sorted.end(),
		[](const Gamepad a, const Gamepad b) {
			return a.getIndex() < b.getIndex();
		}
	);
	gamepads = sorted;
}

void GamepadClient::resetAll()
{
	_resetAllThread = thread([&]() {
		lock = true;
		release();
		init();
		createMaximumGamepads();
		connectAllGamepads();
		lock = false;
		_resetAllThread.detach();
	});
}

void GamepadClient::toggleLock()
{
	lock = !lock;
	reduce([&](Gamepad& pad) {
		pad.clearState();
	});
}


Gamepad GamepadClient::connectNextGamepad()
{
	Gamepad rv;

	bool success = reduceUntilFirst([&](Gamepad& pad) {
		if (pad.connect())
		{
			rv = pad;
			return true;
		}
		return false;
	});

	if (success)
	{
		return rv;
	}
	
	return Gamepad();
}

bool GamepadClient::disconnect(int gamepadIndex)
{
	if (gamepadIndex < 0 || gamepadIndex > gamepads.size())
	{
		return false;
	}
	bool rv = gamepads[gamepadIndex].disconnect();
	return rv;
}

bool GamepadClient::clearOwner(int gamepadIndex)
{
	if (gamepadIndex >= 0 || gamepadIndex < gamepads.size())
	{
		gamepads[gamepadIndex].clearOwner();
		return true;
	}
	return false;
}

void GamepadClient::release()
{
	releaseGamepads();
	if (_client != nullptr)
	{
		vigem_disconnect(_client);
		vigem_free(_client);
		_client = nullptr;
	}
}


Gamepad GamepadClient::getGamepad(int index)
{
	if (index >= 0 && index < gamepads.size())
	{
		return gamepads[index];
	}

	return Gamepad();
}

int GamepadClient::clearAFK(GuestList &guests)
{
	int clearCount = 0;

	reduce([&](Gamepad& pad) {
		if (pad.isOwned())
		{
			Guest guest;
			if (!guests.find(pad.owner.guest.userID, &guest))
			{
				pad.clearOwner();
				clearCount++;
			}
		}
	});

	return clearCount;
}

int GamepadClient::onQuit(Guest& guest)
{
	int result = 0;

	reduce([&](Gamepad& gamepad) {
		if (gamepad.owner.guest.userID == guest.userID)
		{
			gamepad.clearOwner();
			result++;
		}
	});

	return result;
}

void GamepadClient::setLimit(uint32_t guestUserID, uint8_t padLimit)
{
	bool found = findPreferences(guestUserID, [&padLimit](GuestPreferences& prefs) {
		prefs.padLimit = padLimit;
	});

	if (!found)
	{
		guestPreferences.push_back(
			GuestPreferences(guestUserID, padLimit)
		);
	}
}

bool GamepadClient::toggleMirror(uint32_t guestUserID)
{
	bool currentValue = false;

	bool found = findPreferences(guestUserID, [&currentValue](GuestPreferences& prefs) {
		prefs.mirror = !prefs.mirror;
		currentValue = prefs.mirror;
	});

	if(!found)
	{
		GuestPreferences prefs = GuestPreferences(guestUserID, 1, true);
		guestPreferences.push_back(prefs);
		currentValue = prefs.mirror;
	}

	return currentValue;
}

bool GamepadClient::toggleIgnoreDeviceID(uint32_t guestUserID)
{
	bool currentValue = false;

	bool found = findPreferences(guestUserID, [&currentValue](GuestPreferences& prefs) {
		prefs.ignoreDeviceID = !prefs.ignoreDeviceID;
		currentValue = prefs.ignoreDeviceID;
	});

	if (!found)
	{
		GuestPreferences prefs = GuestPreferences(guestUserID, 1, false, false);
		guestPreferences.push_back(prefs);
		currentValue = prefs.ignoreDeviceID;
	}

	return currentValue;
}

const GamepadClient::PICK_REQUEST GamepadClient::pick(Guest guest, int gamepadIndex)
{

	if (gamepadIndex < 0 || gamepadIndex >= gamepads.size())
	{
		return PICK_REQUEST::OUT_OF_RANGE;
	}

	if (!gamepads[gamepadIndex].isConnected())
	{
		return PICK_REQUEST::DISCONNECTED;
	}

	if (gamepads[gamepadIndex].owner.guest.userID == guest.userID)
	{
		return PICK_REQUEST::SAME_USER;
	}

	if (gamepads[gamepadIndex].isOwned())
	{
		return PICK_REQUEST::TAKEN;
	}

	if (isPuppetMaster && gamepads[gamepadIndex].isPuppet)
	{
		return PICK_REQUEST::PUPPET;
	}

	GuestPreferences prefs = GuestPreferences(guest.userID);
	int limit = 1;
	bool found = findPreferences(guest.userID, [&limit](GuestPreferences& prefs) {
		limit = prefs.padLimit;
	});

	if (limit <= 0)
	{
		return PICK_REQUEST::LIMIT_BLOCK;
	}

	bool success = reduceUntilFirst([&](Gamepad& gamepad) {
		if (gamepad.owner.guest.userID == guest.userID)
		{
			gamepads[gamepadIndex].clearState();
			gamepads[gamepadIndex].copyOwner(gamepad);
			gamepad.clearOwner();
			return true;
		}

		return false;
	});

	if (success)
	{
		return PICK_REQUEST::OK;
	}

	return PICK_REQUEST::EMPTY_HANDS;
}


// =============================================================
//
//  Private
//
// =============================================================

bool GamepadClient::sendMessage(Guest guest, ParsecMessage message)
{
	bool success = false;
	uint32_t padId = GAMEPAD_INDEX_ERROR;
	bool isGamepadRequest = false;
	int slots = 0;
	
	GuestPreferences guestPrefs = GuestPreferences();
	findPreferences(guest.userID, [&guestPrefs](GuestPreferences& prefs) {
		guestPrefs = prefs;
	});

	switch (message.type)
	{
	case MESSAGE_GAMEPAD_STATE:
		padId = message.gamepadState.id;
		isGamepadRequest = isRequestState(message);
		if (sendGamepadStateMessage(message.gamepadState, guest, slots, guestPrefs)) { return true; }
		break;

	case MESSAGE_GAMEPAD_AXIS:
		padId = message.gamepadAxis.id;
		if (sendGamepadAxisMessage(message.gamepadAxis, guest, slots, guestPrefs)) { return true; }
		break;

	case MESSAGE_GAMEPAD_BUTTON:
		padId = message.gamepadButton.id;
		isGamepadRequest = isRequestButton(message);
		if (sendGamepadButtonMessage(message.gamepadButton, guest, slots, guestPrefs)) { return true; }
		break;

	case MESSAGE_KEYBOARD:
		padId = 0;
		isGamepadRequest = isRequestKeyboard(message);
		if (sendKeyboardMessage(message.keyboard, guest, slots, guestPrefs)) { return true; }
		break;

	default:
		break;
	}

	if (!success && isGamepadRequest)
	{
		success = tryAssignGamepad(guest, padId, slots, message.type == MESSAGE_KEYBOARD, guestPrefs);
	}

	return success;
}

bool GamepadClient::sendGamepadStateMessage(ParsecGamepadStateMessage& gamepadState, Guest& guest, int &slots, GuestPreferences prefs)
{
	return reduceUntilFirst([&](Gamepad& pad) {
		if (guest.userID == pad.owner.guest.userID)
		{
			slots++;
			if (!(isPuppetMaster && pad.isPuppet) && (prefs.ignoreDeviceID || gamepadState.id == pad.owner.deviceID))
			{
				pad.setStateSafe(toXInput(gamepadState, pad.getState(), prefs));
				return true;
			}
		}
		return false;
	});
}

bool GamepadClient::sendGamepadAxisMessage(ParsecGamepadAxisMessage& gamepadAxis, Guest& guest, int& slots, GuestPreferences prefs)
{
	return reduceUntilFirst([&](Gamepad& pad) {
		if (guest.userID == pad.owner.guest.userID)
		{
			slots++;
			if (!(isPuppetMaster && pad.isPuppet) && (prefs.ignoreDeviceID || gamepadAxis.id == pad.owner.deviceID))
			{
				pad.setStateSafe(toXInput(gamepadAxis, pad.getState(), prefs));
				return true;
			}
		}
		return false;
	});
}

bool GamepadClient::sendGamepadButtonMessage(ParsecGamepadButtonMessage& gamepadButton, Guest& guest, int& slots, GuestPreferences prefs)
{
	return reduceUntilFirst([&](Gamepad& pad) {
		if (guest.userID == pad.owner.guest.userID)
		{
			slots++;
			if (!(isPuppetMaster && pad.isPuppet) && (prefs.ignoreDeviceID || gamepadButton.id == pad.owner.deviceID))
			{
				pad.setStateSafe(toXInput(gamepadButton, pad.getState(), prefs));
				return true;
			}
		}
		return false;
	});
}

bool GamepadClient::sendKeyboardMessage(ParsecKeyboardMessage& keyboard, Guest& guest, int& slots, GuestPreferences prefs)
{
	return reduceUntilFirst([&](Gamepad& pad) {
		if (guest.userID == pad.owner.guest.userID)
		{
			slots++;
			if (!(isPuppetMaster && pad.isPuppet) && (prefs.ignoreDeviceID || pad.owner.isKeyboard))
			{
				pad.setStateSafe(toXInput(keyboard, pad.getKeyboard(), pad.getState(), prefs));
				return true;
			}
		}
		return false;
		});
}

bool GamepadClient::tryAssignGamepad(Guest guest, uint32_t deviceID, int currentSlots, bool isKeyboard, GuestPreferences prefs)
{
	if (currentSlots >= prefs.padLimit)
	{
		return false;
	}
	
	return reduceUntilFirst([&](Gamepad& gamepad) {
		if (!(isPuppetMaster && gamepad.isPuppet) && (gamepad.isAttached() && !gamepad.owner.guest.isValid()))
		{
			gamepad.setOwner(guest, deviceID, isKeyboard);
			return true;
		}

		return false;
	});
}

void GamepadClient::releaseGamepads()
{
	reduce([](Gamepad& pad) {
		pad.release();
	});

	gamepads.clear();
}

void GamepadClient::setMirror(uint32_t guestUserID, bool mirror)
{
	bool found = findPreferences(guestUserID, [&mirror](GuestPreferences& prefs) {
		prefs.mirror = mirror;
	});

	if (!found)
	{
		GuestPreferences prefs = GuestPreferences(guestUserID, 1, mirror);
		guestPreferences.push_back(prefs);
	}
}

void GamepadClient::setIgnoreDeviceID(uint32_t guestUserID, bool ignoreDeviceID)
{
	bool found = findPreferences(guestUserID, [&ignoreDeviceID](GuestPreferences& prefs) {
		prefs.ignoreDeviceID = ignoreDeviceID;
	});

	if (!found)
	{
		GuestPreferences prefs = GuestPreferences(guestUserID, 1, false, ignoreDeviceID);
		guestPreferences.push_back(prefs);
	}
}

bool GamepadClient::isRequestState(ParsecMessage message)
{
	return (message.gamepadState.buttons & (XUSB_GAMEPAD_A | XUSB_GAMEPAD_B | XUSB_GAMEPAD_X | XUSB_GAMEPAD_Y)) != 0;
}

bool GamepadClient::isRequestButton(ParsecMessage message)
{
	return message.gamepadButton.pressed && (
		message.gamepadButton.button == GAMEPAD_BUTTON_A ||
		message.gamepadButton.button == GAMEPAD_BUTTON_B ||
		message.gamepadButton.button == GAMEPAD_BUTTON_X ||
		message.gamepadButton.button == GAMEPAD_BUTTON_Y
		);
}

bool GamepadClient::isRequestKeyboard(ParsecMessage message)
{
	return message.keyboard.pressed && (
		message.keyboard.code == _keyboardMap.A ||
		message.keyboard.code == _keyboardMap.B ||
		message.keyboard.code == _keyboardMap.X ||
		message.keyboard.code == _keyboardMap.Y
	);
}

void GamepadClient::reduce(function<void(Gamepad&)> func)
{
	vector<Gamepad>::iterator gi = gamepads.begin();
	for (; gi != gamepads.end(); ++gi)
	{
		func(*gi);
	}
}

bool GamepadClient::reduceUntilFirst(function<bool(Gamepad&)> func)
{
	vector<Gamepad>::iterator gi = gamepads.begin();
	for (; gi != gamepads.end(); ++gi)
	{
		if (func(*gi))
		{
			return true;
		}
	}

	return false;
}

bool GamepadClient::findPreferences(uint32_t guestUserID, function<void(GamepadClient::GuestPreferences&)> callback)
{
	std::vector<GuestPreferences>::iterator it;
	for (it = guestPreferences.begin(); it != guestPreferences.end(); ++it)
	{
		if ((*it).userID == guestUserID)
		{
			callback(*it);
			return true;
		}
	}

	return false;
}


XINPUT_STATE GamepadClient::toXInput(ParsecGamepadStateMessage& state, XINPUT_STATE previousState, GuestPreferences& prefs)
{
	XINPUT_STATE result = previousState;

	//if (_isAlive && _isConnected && _client != nullptr)
	result.Gamepad.wButtons = state.buttons;
	result.Gamepad.bLeftTrigger = state.leftTrigger;
	result.Gamepad.bRightTrigger = state.rightTrigger;
	result.Gamepad.sThumbLX = state.thumbLX;
	result.Gamepad.sThumbLY = state.thumbLY;
	result.Gamepad.sThumbRX = state.thumbRX;
	result.Gamepad.sThumbRY = state.thumbRY;

	if (prefs.mirror)
	{
		Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_LEFT, state.thumbLX < -GAMEPAD_DEADZONE);
		Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_RIGHT, state.thumbLX > GAMEPAD_DEADZONE);
		Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_UP, state.thumbLY > GAMEPAD_DEADZONE);
		Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_DOWN, state.thumbLY < -GAMEPAD_DEADZONE);
	}

	return result;
}

XINPUT_STATE GamepadClient::toXInput(ParsecGamepadButtonMessage& button, XINPUT_STATE previousState, GuestPreferences& prefs)
{
	XINPUT_STATE result = previousState;
	
	int buttonCode = 0;

	switch (button.button)
	{
	case GAMEPAD_BUTTON_A:
		buttonCode = XUSB_GAMEPAD_A;
		break;
	case GAMEPAD_BUTTON_B:
		buttonCode = XUSB_GAMEPAD_B;
		break;
	case GAMEPAD_BUTTON_X:
		buttonCode = XUSB_GAMEPAD_X;
		break;
	case GAMEPAD_BUTTON_Y:
		buttonCode = XUSB_GAMEPAD_Y;
		break;
	case GAMEPAD_BUTTON_BACK:
		buttonCode = XUSB_GAMEPAD_BACK;
		break;
	case GAMEPAD_BUTTON_GUIDE:
		buttonCode = XUSB_GAMEPAD_GUIDE;
		break;
	case GAMEPAD_BUTTON_START:
		buttonCode = XUSB_GAMEPAD_START;
		break;
	case GAMEPAD_BUTTON_LSTICK:
		buttonCode = XUSB_GAMEPAD_LEFT_THUMB;
		break;
	case GAMEPAD_BUTTON_RSTICK:
		buttonCode = XUSB_GAMEPAD_RIGHT_THUMB;
		break;
	case GAMEPAD_BUTTON_LSHOULDER:
		buttonCode = XUSB_GAMEPAD_LEFT_SHOULDER;
		break;
	case GAMEPAD_BUTTON_RSHOULDER:
		buttonCode = XUSB_GAMEPAD_RIGHT_SHOULDER;
		break;
	case GAMEPAD_BUTTON_DPAD_UP:
		buttonCode = XUSB_GAMEPAD_DPAD_UP;
		break;
	case GAMEPAD_BUTTON_DPAD_DOWN:
		buttonCode = XUSB_GAMEPAD_DPAD_DOWN;
		break;
	case GAMEPAD_BUTTON_DPAD_LEFT:
		buttonCode = XUSB_GAMEPAD_DPAD_LEFT;
		break;
	case GAMEPAD_BUTTON_DPAD_RIGHT:
		buttonCode = XUSB_GAMEPAD_DPAD_RIGHT;
		break;
	default:
		break;
	}

	Bitwise::setValue(&result.Gamepad.wButtons, buttonCode, button.pressed);

	return result;
}

XINPUT_STATE GamepadClient::toXInput(ParsecGamepadAxisMessage& axis, XINPUT_STATE previousState, GuestPreferences& prefs)
{
	XINPUT_STATE result = previousState;

	switch (axis.axis)
	{
	case GAMEPAD_AXIS_LX:
		result.Gamepad.sThumbLX = axis.value;
		if (prefs.mirror)
		{
			Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_LEFT, axis.value < -GAMEPAD_DEADZONE);
			Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_RIGHT, axis.value > GAMEPAD_DEADZONE);
		}
		break;
	case GAMEPAD_AXIS_LY:
		result.Gamepad.sThumbLY = -axis.value;
		if (prefs.mirror)
		{
			Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_UP, axis.value < -GAMEPAD_DEADZONE);
			Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_DOWN, axis.value > GAMEPAD_DEADZONE);
		}
		break;
	case GAMEPAD_AXIS_RX:
		result.Gamepad.sThumbRX = axis.value;
		break;
	case GAMEPAD_AXIS_RY:
		result.Gamepad.sThumbRY = -axis.value;
		break;
	case GAMEPAD_AXIS_TRIGGERL:
		result.Gamepad.bLeftTrigger = axis.value;
		break;
	case GAMEPAD_AXIS_TRIGGERR:
		result.Gamepad.bRightTrigger = axis.value;
		break;
	default:
		break;
	}

	return result;
}

XINPUT_STATE GamepadClient::toXInput(ParsecKeyboardMessage& key, Gamepad::Keyboard& keyboard, XINPUT_STATE previousState, GuestPreferences& prefs)
{
	XINPUT_STATE result = previousState;

	// Left Stick
	if (key.code == _keyboardMap.LLeft)
	{
		if (key.pressed) result.Gamepad.sThumbLX = GAMEPAD_STICK_MIN;
		else result.Gamepad.sThumbLX = keyboard.LRight ? GAMEPAD_STICK_MAX : 0;
		if (prefs.mirror) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_LEFT, key.pressed);
		keyboard.LLeft = key.pressed;
	}

	if (key.code == _keyboardMap.LRight)
	{
		if (key.pressed) result.Gamepad.sThumbLX = GAMEPAD_STICK_MAX;
		else result.Gamepad.sThumbLX = keyboard.LLeft ? GAMEPAD_STICK_MIN : 0;
		if (prefs.mirror) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_RIGHT, key.pressed);
		keyboard.LRight = key.pressed;
	}

	if (key.code == _keyboardMap.LUp)
	{
		if (key.pressed) result.Gamepad.sThumbLY = GAMEPAD_STICK_MAX;
		else result.Gamepad.sThumbLY = keyboard.LDown ? GAMEPAD_STICK_MIN : 0;
		if (prefs.mirror) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_UP, key.pressed);
		keyboard.LUp = key.pressed;
	}

	if (key.code == _keyboardMap.LDown)
	{
		if (key.pressed) result.Gamepad.sThumbLY = GAMEPAD_STICK_MIN;
		else result.Gamepad.sThumbLY = keyboard.LUp ? GAMEPAD_STICK_MAX : 0;
		if (prefs.mirror) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_DOWN, key.pressed);
		keyboard.LDown = key.pressed;
	}

	// Right Stick
	if (key.code == _keyboardMap.RLeft)
	{
		if (key.pressed) result.Gamepad.sThumbRX = GAMEPAD_STICK_MIN;
		else result.Gamepad.sThumbRX = keyboard.RRight ? GAMEPAD_STICK_MAX : 0;
		keyboard.RLeft = key.pressed;
	}

	if (key.code == _keyboardMap.RRight)
	{
		if (key.pressed) result.Gamepad.sThumbRX = GAMEPAD_STICK_MAX;
		else result.Gamepad.sThumbRX = keyboard.RLeft ? GAMEPAD_STICK_MIN : 0;
		keyboard.RRight = key.pressed;
	}

	if (key.code == _keyboardMap.RUp)
	{
		if (key.pressed) result.Gamepad.sThumbRY = GAMEPAD_STICK_MAX;
		else result.Gamepad.sThumbRY = keyboard.RDown ? GAMEPAD_STICK_MIN : 0;
		keyboard.RUp = key.pressed;
	}

	if (key.code == _keyboardMap.RDown)
	{
		if (key.pressed) result.Gamepad.sThumbRY = GAMEPAD_STICK_MIN;
		else result.Gamepad.sThumbRY = keyboard.RUp ? GAMEPAD_STICK_MAX : 0;
		keyboard.RDown = key.pressed;
	}

	// DPad
	if (key.code == _keyboardMap.DLeft)  Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_LEFT, key.pressed);
	if (key.code == _keyboardMap.DRight) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_RIGHT, key.pressed);
	if (key.code == _keyboardMap.DUp)    Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_UP, key.pressed);
	if (key.code == _keyboardMap.DDown)  Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_DPAD_DOWN, key.pressed);

	// Face buttons
	if (key.code == _keyboardMap.A) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_A, key.pressed);
	if (key.code == _keyboardMap.B) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_B, key.pressed);
	if (key.code == _keyboardMap.X) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_X, key.pressed);
	if (key.code == _keyboardMap.Y) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_Y, key.pressed);

	// Center
	if (key.code == _keyboardMap.Back)  Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_BACK, key.pressed);
	if (key.code == _keyboardMap.Start) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_START, key.pressed);

	// Shoulders
	if (key.code == _keyboardMap.LB) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_LEFT_SHOULDER, key.pressed);
	if (key.code == _keyboardMap.RB) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_RIGHT_SHOULDER, key.pressed);

	// Triggers
	if (key.code == _keyboardMap.LT) result.Gamepad.bLeftTrigger = (key.pressed ? GAMEPAD_STICK_MAX : GAMEPAD_STICK_MIN);
	if (key.code == _keyboardMap.RT) result.Gamepad.bRightTrigger = (key.pressed ? GAMEPAD_STICK_MAX : GAMEPAD_STICK_MIN);

	// Thumbs
	if (key.code == _keyboardMap.LThumb) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_LEFT_THUMB, key.pressed);
	if (key.code == _keyboardMap.RThumb) Bitwise::setValue(&result.Gamepad.wButtons, XUSB_GAMEPAD_RIGHT_THUMB, key.pressed);

	return result;
}