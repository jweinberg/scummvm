/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * $URL$
 * $Id$
 *
 */

#if !defined(BACKEND_EVENTS_SDL_GP2X_H) && !defined(DISABLE_DEFAULT_EVENTMANAGER)
#define BACKEND_EVENTS_SDL_GP2X_H

#include "backends/events/sdl/sdl-events.h"

/**
 * SDL events manager for GP2X
 */
class GP2XSdlEventSource : public SdlEventSource {
public:
	GP2XSdlEventSource();

protected:
	bool _stickBtn[32];

	/** Button state for L button modifier */
	bool _buttonStateL;

	/**
	 * Handles the stick movement
	 */
	void moveStick();

	virtual bool handleKeyDown(SDL_Event &ev, Common::Event &event);
	virtual bool handleJoyButtonDown(SDL_Event &ev, Common::Event &event);
	virtual bool handleJoyButtonUp(SDL_Event &ev, Common::Event &event);
	virtual bool handleJoyAxisMotion(SDL_Event &ev, Common::Event &event);

	virtual void SDLModToOSystemKeyFlags(SDLMod mod, Common::Event &event);

	virtual bool remapKey(SDL_Event &ev, Common::Event &event);
};

#endif
