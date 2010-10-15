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

/*
 * This code is based on Broken Sword 2.5 engine
 *
 * Copyright (c) Malte Thiesen, Daniel Queteschiner and Michael Elsdoerfer
 *
 * Licensed under GNU GPL v2
 *
 */

#include "sword25/gfx/animation.h"

#include "sword25/kernel/kernel.h"
#include "sword25/kernel/resmanager.h"
#include "sword25/kernel/inputpersistenceblock.h"
#include "sword25/kernel/outputpersistenceblock.h"
#include "sword25/kernel/callbackregistry.h"
#include "sword25/package/packagemanager.h"
#include "sword25/gfx/image/image.h"
#include "sword25/gfx/animationtemplate.h"
#include "sword25/gfx/animationtemplateregistry.h"
#include "sword25/gfx/animationresource.h"
#include "sword25/gfx/bitmapresource.h"
#include "sword25/gfx/graphicengine.h"

namespace Sword25 {

#define BS_LOG_PREFIX "ANIMATION"

Animation::Animation(RenderObjectPtr<RenderObject> parentPtr, const Common::String &fileName) :
	TimedRenderObject(parentPtr, RenderObject::TYPE_ANIMATION) {
	// Das BS_RenderObject konnte nicht erzeugt werden, daher muss an dieser Stelle abgebrochen werden.
	if (!_initSuccess)
		return;

	initMembers();

	// Vom negativen Fall ausgehen.
	_initSuccess = false;

	initializeAnimationResource(fileName);

	// Erfolg signalisieren.
	_initSuccess = true;
}

Animation::Animation(RenderObjectPtr<RenderObject> parentPtr, const AnimationTemplate &templ) :
	TimedRenderObject(parentPtr, RenderObject::TYPE_ANIMATION) {
	// Das BS_RenderObject konnte nicht erzeugt werden, daher muss an dieser Stelle abgebrochen werden.
	if (!_initSuccess)
		return;

	initMembers();

	// Vom negativen Fall ausgehen.
	_initSuccess = false;

	_animationTemplateHandle = AnimationTemplate::create(templ);

	// Erfolg signalisieren.
	_initSuccess = true;
}

Animation::Animation(InputPersistenceBlock &reader, RenderObjectPtr<RenderObject> parentPtr, uint handle) :
	TimedRenderObject(parentPtr, RenderObject::TYPE_ANIMATION, handle) {
	// Das BS_RenderObject konnte nicht erzeugt werden, daher muss an dieser Stelle abgebrochen werden.
	if (!_initSuccess)
		return;

	initMembers();

	// Objekt vom Stream laden.
	_initSuccess = unpersist(reader);
}

void Animation::initializeAnimationResource(const Common::String &fileName) {
	// Die Resource wird f�r die gesamte Lebensdauer des Animations-Objektes gelockt.
	Resource *resourcePtr = Kernel::GetInstance()->GetResourceManager()->RequestResource(fileName);
	if (resourcePtr && resourcePtr->GetType() == Resource::TYPE_ANIMATION)
		_animationResourcePtr = static_cast<AnimationResource *>(resourcePtr);
	else {
		BS_LOG_ERRORLN("The resource \"%s\" could not be requested. The Animation can't be created.", fileName.c_str());
		return;
	}

	// Gr��e und Position der Animation anhand des aktuellen Frames bestimmen.
	computeCurrentCharacteristics();
}

void Animation::initMembers() {
	_currentFrame = 0;
	_currentFrameTime = 0;
	_direction = FORWARD;
	_running = false;
	_finished = false;
	_relX = 0;
	_relY = 0;
	_scaleFactorX = 1.0f;
	_scaleFactorY = 1.0f;
	_modulationColor = 0xffffffff;
	_animationResourcePtr = 0;
	_animationTemplateHandle = 0;
	_framesLocked = false;
}

Animation::~Animation() {
	if (getAnimationDescription()) {
		stop();
		getAnimationDescription()->unlock();
	}

	// Delete Callbacks
	Common::Array<ANIMATION_CALLBACK_DATA>::iterator it = _deleteCallbacks.begin();
	for (; it != _deleteCallbacks.end(); it++)((*it).Callback)((*it).Data);

}

void Animation::play() {
	// Wenn die Animation zuvor komplett durchgelaufen ist, wird sie wieder von Anfang abgespielt
	if (_finished)
		stop();

	_running = true;
	lockAllFrames();
}

void Animation::pause() {
	_running = false;
	unlockAllFrames();
}

void Animation::stop() {
	_currentFrame = 0;
	_currentFrameTime = 0;
	_direction = FORWARD;
	pause();
}

void Animation::setFrame(uint nr) {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);

	if (nr >= animationDescriptionPtr->getFrameCount()) {
		BS_LOG_ERRORLN("Tried to set animation to illegal frame (%d). Value must be between 0 and %d.",
		               nr, animationDescriptionPtr->getFrameCount());
		return;
	}

	_currentFrame = nr;
	_currentFrameTime = 0;
	computeCurrentCharacteristics();
	forceRefresh();
}

bool Animation::doRender() {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	BS_ASSERT(_currentFrame < animationDescriptionPtr->getFrameCount());

	// Bitmap des aktuellen Frames holen
	Resource *pResource = Kernel::GetInstance()->GetResourceManager()->RequestResource(animationDescriptionPtr->getFrame(_currentFrame).fileName);
	BS_ASSERT(pResource);
	BS_ASSERT(pResource->GetType() == Resource::TYPE_BITMAP);
	BitmapResource *pBitmapResource = static_cast<BitmapResource *>(pResource);

	// Framebufferobjekt holen
	GraphicEngine *pGfx = Kernel::GetInstance()->GetGfx();
	BS_ASSERT(pGfx);

	// Bitmap zeichnen
	bool result;
	if (isScalingAllowed() && (_width != pBitmapResource->getWidth() || _height != pBitmapResource->getHeight())) {
		result = pBitmapResource->blit(_absoluteX, _absoluteY,
		                               (animationDescriptionPtr->getFrame(_currentFrame).flipV ? BitmapResource::FLIP_V : 0) |
		                               (animationDescriptionPtr->getFrame(_currentFrame).flipH ? BitmapResource::FLIP_H : 0),
		                               0, _modulationColor, _width, _height);
	} else {
		result = pBitmapResource->blit(_absoluteX, _absoluteY,
		                               (animationDescriptionPtr->getFrame(_currentFrame).flipV ? BitmapResource::FLIP_V : 0) |
		                               (animationDescriptionPtr->getFrame(_currentFrame).flipH ? BitmapResource::FLIP_H : 0),
		                               0, _modulationColor, -1, -1);
	}

	// Resource freigeben
	pBitmapResource->release();

	return result;
}

void Animation::frameNotification(int timeElapsed) {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	BS_ASSERT(timeElapsed >= 0);

	// Nur wenn die Animation l�uft wird sie auch weiterbewegt
	if (_running) {
		// Gesamte vergangene Zeit bestimmen (inkl. Restzeit des aktuellen Frames)
		_currentFrameTime += timeElapsed;

		// Anzahl an zu �berpringenden Frames bestimmen
		int skipFrames = animationDescriptionPtr->getMillisPerFrame() == 0 ? 0 : _currentFrameTime / animationDescriptionPtr->getMillisPerFrame();

		// Neue Frame-Restzeit bestimmen
		_currentFrameTime -= animationDescriptionPtr->getMillisPerFrame() * skipFrames;

		// Neuen Frame bestimmen (je nach aktuellener Abspielrichtung wird addiert oder subtrahiert)
		int tmpCurFrame = _currentFrame;
		switch (_direction) {
		case FORWARD:
			tmpCurFrame += skipFrames;
			break;

		case BACKWARD:
			tmpCurFrame -= skipFrames;
			break;

		default:
			BS_ASSERT(0);
		}

		// �berl�ufe behandeln
		if (tmpCurFrame < 0) {
			// Loop-Point Callbacks
			for (uint i = 0; i < _loopPointCallbacks.size();) {
				if ((_loopPointCallbacks[i].Callback)(_loopPointCallbacks[i].Data) == false) {
					_loopPointCallbacks.remove_at(i);
				} else
					i++;
			}

			// Ein Unterlauf darf nur auftreten, wenn der Animationstyp JOJO ist.
			BS_ASSERT(animationDescriptionPtr->getAnimationType() == AT_JOJO);
			tmpCurFrame = - tmpCurFrame;
			_direction = FORWARD;
		} else if (static_cast<uint>(tmpCurFrame) >= animationDescriptionPtr->getFrameCount()) {
			// Loop-Point Callbacks
			for (uint i = 0; i < _loopPointCallbacks.size();) {
				if ((_loopPointCallbacks[i].Callback)(_loopPointCallbacks[i].Data) == false) {
					_loopPointCallbacks.remove_at(i);
				} else
					i++;
			}

			switch (animationDescriptionPtr->getAnimationType()) {
			case AT_ONESHOT:
				tmpCurFrame = animationDescriptionPtr->getFrameCount() - 1;
				_finished = true;
				pause();
				break;

			case AT_LOOP:
				tmpCurFrame = tmpCurFrame % animationDescriptionPtr->getFrameCount();
				break;

			case AT_JOJO:
				tmpCurFrame = animationDescriptionPtr->getFrameCount() - (tmpCurFrame % animationDescriptionPtr->getFrameCount()) - 1;
				_direction = BACKWARD;
				break;

			default:
				BS_ASSERT(0);
			}
		}

		if ((int)_currentFrame != tmpCurFrame) {
			forceRefresh();

			if (animationDescriptionPtr->getFrame(_currentFrame).action != "") {
				// Action Callbacks
				for (uint i = 0; i < _actionCallbacks.size();) {
					if ((_actionCallbacks[i].Callback)(_actionCallbacks[i].Data) == false) {
						_actionCallbacks.remove_at(i);
					} else
						i++;
				}
			}
		}

		_currentFrame = static_cast<uint>(tmpCurFrame);
	}

	// Gr��e und Position der Animation anhand des aktuellen Frames bestimmen
	computeCurrentCharacteristics();

	BS_ASSERT(_currentFrame < animationDescriptionPtr->getFrameCount());
	BS_ASSERT(_currentFrameTime >= 0);
}

void Animation::computeCurrentCharacteristics() {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	const AnimationResource::Frame &curFrame = animationDescriptionPtr->getFrame(_currentFrame);

	Resource *pResource = Kernel::GetInstance()->GetResourceManager()->RequestResource(curFrame.fileName);
	BS_ASSERT(pResource);
	BS_ASSERT(pResource->GetType() == Resource::TYPE_BITMAP);
	BitmapResource *pBitmap = static_cast<BitmapResource *>(pResource);

	// Gr��e des Bitmaps auf die Animation �bertragen
	_width = static_cast<int>(pBitmap->getWidth() * _scaleFactorX);
	_height = static_cast<int>(pBitmap->getHeight() * _scaleFactorY);

	// Position anhand des Hotspots berechnen und setzen
	int posX = _relX + computeXModifier();
	int posY = _relY + computeYModifier();

	RenderObject::setPos(posX, posY);

	pBitmap->release();
}

bool Animation::lockAllFrames() {
	if (!_framesLocked) {
		AnimationDescription *animationDescriptionPtr = getAnimationDescription();
		BS_ASSERT(animationDescriptionPtr);
		for (uint i = 0; i < animationDescriptionPtr->getFrameCount(); ++i) {
			if (!Kernel::GetInstance()->GetResourceManager()->RequestResource(animationDescriptionPtr->getFrame(i).fileName)) {
				BS_LOG_ERRORLN("Could not lock all animation frames.");
				return false;
			}
		}

		_framesLocked = true;
	}

	return true;
}

bool Animation::unlockAllFrames() {
	if (_framesLocked) {
		AnimationDescription *animationDescriptionPtr = getAnimationDescription();
		BS_ASSERT(animationDescriptionPtr);
		for (uint i = 0; i < animationDescriptionPtr->getFrameCount(); ++i) {
			Resource *pResource;
			if (!(pResource = Kernel::GetInstance()->GetResourceManager()->RequestResource(animationDescriptionPtr->getFrame(i).fileName))) {
				BS_LOG_ERRORLN("Could not unlock all animation frames.");
				return false;
			}

			// Zwei mal freigeben um den Request von LockAllFrames() und den jetzigen Request aufzuheben
			pResource->release();
			if (pResource->GetLockCount())
				pResource->release();
		}

		_framesLocked = false;
	}

	return true;
}

Animation::ANIMATION_TYPES Animation::getAnimationType() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->getAnimationType();
}

int Animation::getFPS() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->getFPS();
}

int Animation::getFrameCount() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->getFrameCount();
}

bool Animation::isScalingAllowed() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->isScalingAllowed();
}

bool Animation::isAlphaAllowed() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->isAlphaAllowed();
}

bool Animation::isColorModulationAllowed() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->isColorModulationAllowed();
}

void Animation::setPos(int relX, int relY) {
	_relX = relX;
	_relY = relY;

	computeCurrentCharacteristics();
}

void Animation::setX(int relX) {
	_relX = relX;

	computeCurrentCharacteristics();
}

void Animation::setY(int relY) {
	_relY = relY;

	computeCurrentCharacteristics();
}

void Animation::setAlpha(int alpha) {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	if (!animationDescriptionPtr->isAlphaAllowed()) {
		BS_LOG_WARNINGLN("Tried to set alpha value on an animation that does not support alpha. Call was ignored.");
		return;
	}

	uint newModulationColor = (_modulationColor & 0x00ffffff) | alpha << 24;
	if (newModulationColor != _modulationColor) {
		_modulationColor = newModulationColor;
		forceRefresh();
	}
}

void Animation::setModulationColor(uint modulationColor) {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	if (!animationDescriptionPtr->isColorModulationAllowed()) {
		BS_LOG_WARNINGLN("Tried to set modulation color on an animation that does not support color modulation. Call was ignored");
		return;
	}

	uint newModulationColor = (modulationColor & 0x00ffffff) | (_modulationColor & 0xff000000);
	if (newModulationColor != _modulationColor) {
		_modulationColor = newModulationColor;
		forceRefresh();
	}
}

void Animation::setScaleFactor(float scaleFactor) {
	setScaleFactorX(scaleFactor);
	setScaleFactorY(scaleFactor);
}

void Animation::setScaleFactorX(float scaleFactorX) {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	if (!animationDescriptionPtr->isScalingAllowed()) {
		BS_LOG_WARNINGLN("Tried to set x scale factor on an animation that does not support scaling. Call was ignored");
		return;
	}

	if (scaleFactorX != _scaleFactorX) {
		_scaleFactorX = scaleFactorX;
		if (_scaleFactorX <= 0.0f)
			_scaleFactorX = 0.001f;
		forceRefresh();
		computeCurrentCharacteristics();
	}
}

void Animation::setScaleFactorY(float scaleFactorY) {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	if (!animationDescriptionPtr->isScalingAllowed()) {
		BS_LOG_WARNINGLN("Tried to set y scale factor on an animation that does not support scaling. Call was ignored");
		return;
	}

	if (scaleFactorY != _scaleFactorY) {
		_scaleFactorY = scaleFactorY;
		if (_scaleFactorY <= 0.0f)
			_scaleFactorY = 0.001f;
		forceRefresh();
		computeCurrentCharacteristics();
	}
}

const Common::String &Animation::getCurrentAction() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	return animationDescriptionPtr->getFrame(_currentFrame).action;
}

int Animation::getX() const {
	return _relX;
}

int Animation::getY() const {
	return _relY;
}

int Animation::getAbsoluteX() const {
	return _absoluteX + (_relX - _x);
}

int Animation::getAbsoluteY() const {
	return _absoluteY + (_relY - _y);
}

int Animation::computeXModifier() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	const AnimationResource::Frame &curFrame = animationDescriptionPtr->getFrame(_currentFrame);

	Resource *pResource = Kernel::GetInstance()->GetResourceManager()->RequestResource(curFrame.fileName);
	BS_ASSERT(pResource);
	BS_ASSERT(pResource->GetType() == Resource::TYPE_BITMAP);
	BitmapResource *pBitmap = static_cast<BitmapResource *>(pResource);

	int result = curFrame.flipV ? - static_cast<int>((pBitmap->getWidth() - 1 - curFrame.hotspotX) * _scaleFactorX) :
	             - static_cast<int>(curFrame.hotspotX * _scaleFactorX);

	pBitmap->release();

	return result;
}

int Animation::computeYModifier() const {
	AnimationDescription *animationDescriptionPtr = getAnimationDescription();
	BS_ASSERT(animationDescriptionPtr);
	const AnimationResource::Frame &curFrame = animationDescriptionPtr->getFrame(_currentFrame);

	Resource *pResource = Kernel::GetInstance()->GetResourceManager()->RequestResource(curFrame.fileName);
	BS_ASSERT(pResource);
	BS_ASSERT(pResource->GetType() == Resource::TYPE_BITMAP);
	BitmapResource *pBitmap = static_cast<BitmapResource *>(pResource);

	int result = curFrame.flipH ? - static_cast<int>((pBitmap->getHeight() - 1 - curFrame.hotspotY) * _scaleFactorY) :
	             - static_cast<int>(curFrame.hotspotY * _scaleFactorY);

	pBitmap->release();

	return result;
}

void Animation::registerActionCallback(ANIMATION_CALLBACK callback, uint data) {
	ANIMATION_CALLBACK_DATA cd;
	cd.Callback = callback;
	cd.Data = data;
	_actionCallbacks.push_back(cd);
}

void Animation::registerLoopPointCallback(ANIMATION_CALLBACK callback, uint data) {
	ANIMATION_CALLBACK_DATA cd;
	cd.Callback = callback;
	cd.Data = data;
	_loopPointCallbacks.push_back(cd);
}

void Animation::registerDeleteCallback(ANIMATION_CALLBACK callback, uint data) {
	ANIMATION_CALLBACK_DATA cd;
	cd.Callback = callback;
	cd.Data = data;
	_deleteCallbacks.push_back(cd);
}

void Animation::persistCallbackVector(OutputPersistenceBlock &writer, const Common::Array<ANIMATION_CALLBACK_DATA> &vector) {
	// Anzahl an Callbacks persistieren.
	writer.write(vector.size());

	// Alle Callbacks einzeln persistieren.
	Common::Array<ANIMATION_CALLBACK_DATA>::const_iterator it = vector.begin();
	while (it != vector.end()) {
		writer.write(CallbackRegistry::instance().resolveCallbackPointer((void (*)(int))it->Callback));
		writer.write(it->Data);

		++it;
	}
}

void Animation::unpersistCallbackVector(InputPersistenceBlock &reader, Common::Array<ANIMATION_CALLBACK_DATA> &vector) {
	// Callbackvector leeren.
	vector.resize(0);

	// Anzahl an Callbacks einlesen.
	uint callbackCount;
	reader.read(callbackCount);

	// Alle Callbacks einzeln wieder herstellen.
	for (uint i = 0; i < callbackCount; ++i) {
		ANIMATION_CALLBACK_DATA callbackData;

		Common::String callbackFunctionName;
		reader.read(callbackFunctionName);
		callbackData.Callback = reinterpret_cast<ANIMATION_CALLBACK>(CallbackRegistry::instance().resolveCallbackFunction(callbackFunctionName));

		reader.read(callbackData.Data);

		vector.push_back(callbackData);
	}
}

bool Animation::persist(OutputPersistenceBlock &writer) {
	bool result = true;

	result &= RenderObject::persist(writer);

	writer.write(_relX);
	writer.write(_relY);
	writer.write(_scaleFactorX);
	writer.write(_scaleFactorY);
	writer.write(_modulationColor);
	writer.write(_currentFrame);
	writer.write(_currentFrameTime);
	writer.write(_running);
	writer.write(_finished);
	writer.write(static_cast<uint>(_direction));

	// Je nach Animationstyp entweder das Template oder die Ressource speichern.
	if (_animationResourcePtr) {
		uint marker = 0;
		writer.write(marker);
		writer.write(_animationResourcePtr->getFileName());
	} else if (_animationTemplateHandle) {
		uint marker = 1;
		writer.write(marker);
		writer.write(_animationTemplateHandle);
	} else {
		BS_ASSERT(false);
	}

	//writer.write(_AnimationDescriptionPtr);

	writer.write(_framesLocked);
	persistCallbackVector(writer, _loopPointCallbacks);
	persistCallbackVector(writer, _actionCallbacks);
	persistCallbackVector(writer, _deleteCallbacks);

	result &= RenderObject::persistChildren(writer);

	return result;
}

// -----------------------------------------------------------------------------

bool Animation::unpersist(InputPersistenceBlock &reader) {
	bool result = true;

	result &= RenderObject::unpersist(reader);

	reader.read(_relX);
	reader.read(_relY);
	reader.read(_scaleFactorX);
	reader.read(_scaleFactorY);
	reader.read(_modulationColor);
	reader.read(_currentFrame);
	reader.read(_currentFrameTime);
	reader.read(_running);
	reader.read(_finished);
	uint direction;
	reader.read(direction);
	_direction = static_cast<Direction>(direction);

	// Animationstyp einlesen.
	uint marker;
	reader.read(marker);
	if (marker == 0) {
		Common::String resourceFilename;
		reader.read(resourceFilename);
		initializeAnimationResource(resourceFilename);
	} else if (marker == 1) {
		reader.read(_animationTemplateHandle);
	} else {
		BS_ASSERT(false);
	}

	reader.read(_framesLocked);
	if (_framesLocked)
		lockAllFrames();

	unpersistCallbackVector(reader, _loopPointCallbacks);
	unpersistCallbackVector(reader, _actionCallbacks);
	unpersistCallbackVector(reader, _deleteCallbacks);

	result &= RenderObject::unpersistChildren(reader);

	return reader.isGood() && result;
}

// -----------------------------------------------------------------------------

AnimationDescription *Animation::getAnimationDescription() const {
	if (_animationResourcePtr)
		return _animationResourcePtr;
	else
		return AnimationTemplateRegistry::instance().resolveHandle(_animationTemplateHandle);
}

} // End of namespace Sword25