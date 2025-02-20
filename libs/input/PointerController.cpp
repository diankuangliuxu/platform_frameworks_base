/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "PointerController"
//#define LOG_NDEBUG 0

// Log debug messages about pointer updates
#define DEBUG_POINTER_UPDATES 0

#include "PointerController.h"

#include <log/log.h>

#include <memory>

namespace android {

// --- PointerController ---

// Time to wait before starting the fade when the pointer is inactive.
static const nsecs_t INACTIVITY_TIMEOUT_DELAY_TIME_NORMAL = 15 * 1000 * 1000000LL; // 15 seconds
static const nsecs_t INACTIVITY_TIMEOUT_DELAY_TIME_SHORT = 3 * 1000 * 1000000LL; // 3 seconds

// Time to spend fading out the spot completely.
static const nsecs_t SPOT_FADE_DURATION = 200 * 1000000LL; // 200 ms

// Time to spend fading out the pointer completely.
static const nsecs_t POINTER_FADE_DURATION = 500 * 1000000LL; // 500 ms

// The number of events to be read at once for DisplayEventReceiver.
static const int EVENT_BUFFER_SIZE = 100;

std::shared_ptr<PointerController> PointerController::create(
        const sp<PointerControllerPolicyInterface>& policy, const sp<Looper>& looper,
        const sp<SpriteController>& spriteController) {
    std::shared_ptr<PointerController> controller = std::shared_ptr<PointerController>(
            new PointerController(policy, looper, spriteController));

    /*
     * Now we need to hook up the constructed PointerController object to its callbacks.
     *
     * This must be executed after the constructor but before any other methods on PointerController
     * in order to ensure that the fully constructed object is visible on the Looper thread, since
     * that may be a different thread than where the PointerController is initially constructed.
     *
     * Unfortunately, this cannot be done as part of the constructor since we need to hand out
     * weak_ptr's which themselves cannot be constructed until there's at least one shared_ptr.
     */

    controller->mHandler->pointerController = controller;
    controller->mCallback->pointerController = controller;
    if (controller->mDisplayEventReceiver.initCheck() == NO_ERROR) {
        controller->mLooper->addFd(controller->mDisplayEventReceiver.getFd(), Looper::POLL_CALLBACK,
                                   Looper::EVENT_INPUT, controller->mCallback, nullptr);
    } else {
        ALOGE("Failed to initialize DisplayEventReceiver.");
    }
    return controller;
}

PointerController::PointerController(const sp<PointerControllerPolicyInterface>& policy,
                                     const sp<Looper>& looper,
                                     const sp<SpriteController>& spriteController)
      : mPolicy(policy),
        mLooper(looper),
        mSpriteController(spriteController),
        mHandler(new MessageHandler()),
        mCallback(new LooperCallback()) {
    AutoMutex _l(mLock);

    mLocked.animationPending = false;

    mLocked.presentation = Presentation::POINTER;
    mLocked.presentationChanged = false;

    mLocked.inactivityTimeout = InactivityTimeout::NORMAL;

    mLocked.pointerFadeDirection = 0;
    mLocked.pointerX = 0;
    mLocked.pointerY = 0;
    mLocked.pointerAlpha = 0.0f; // pointer is initially faded
    mLocked.pointerSprite = mSpriteController->createSprite();
    mLocked.pointerIconChanged = false;
    mLocked.requestedPointerType = mPolicy->getDefaultPointerIconId();

    mLocked.animationFrameIndex = 0;
    mLocked.lastFrameUpdatedTime = 0;

    mLocked.buttonState = 0;
}

PointerController::~PointerController() {
    mLooper->removeMessages(mHandler);

    AutoMutex _l(mLock);

    mLocked.pointerSprite.clear();

    for (auto& it : mLocked.spotsByDisplay) {
        const std::vector<Spot*>& spots = it.second;
        size_t numSpots = spots.size();
        for (size_t i = 0; i < numSpots; i++) {
            delete spots[i];
        }
    }
    mLocked.spotsByDisplay.clear();
    mLocked.recycledSprites.clear();
}

bool PointerController::getBounds(float* outMinX, float* outMinY,
        float* outMaxX, float* outMaxY) const {
    AutoMutex _l(mLock);

    return getBoundsLocked(outMinX, outMinY, outMaxX, outMaxY);
}

bool PointerController::getBoundsLocked(float* outMinX, float* outMinY,
        float* outMaxX, float* outMaxY) const {

    if (!mLocked.viewport.isValid()) {
        return false;
    }

    *outMinX = mLocked.viewport.logicalLeft;
    *outMinY = mLocked.viewport.logicalTop;
    *outMaxX = mLocked.viewport.logicalRight - 1;
    *outMaxY = mLocked.viewport.logicalBottom - 1;
    return true;
}

void PointerController::move(float deltaX, float deltaY) {
#if DEBUG_POINTER_UPDATES
    ALOGD("Move pointer by deltaX=%0.3f, deltaY=%0.3f", deltaX, deltaY);
#endif
    if (deltaX == 0.0f && deltaY == 0.0f) {
        return;
    }

    AutoMutex _l(mLock);

    setPositionLocked(mLocked.pointerX + deltaX, mLocked.pointerY + deltaY);
}

void PointerController::setButtonState(int32_t buttonState) {
#if DEBUG_POINTER_UPDATES
    ALOGD("Set button state 0x%08x", buttonState);
#endif
    AutoMutex _l(mLock);

    if (mLocked.buttonState != buttonState) {
        mLocked.buttonState = buttonState;
    }
}

int32_t PointerController::getButtonState() const {
    AutoMutex _l(mLock);

    return mLocked.buttonState;
}

void PointerController::setPosition(float x, float y) {
#if DEBUG_POINTER_UPDATES
    ALOGD("Set pointer position to x=%0.3f, y=%0.3f", x, y);
#endif
    AutoMutex _l(mLock);

    setPositionLocked(x, y);
}

void PointerController::setPositionLocked(float x, float y) {
    float minX, minY, maxX, maxY;
    if (getBoundsLocked(&minX, &minY, &maxX, &maxY)) {
        if (x <= minX) {
            mLocked.pointerX = minX;
        } else if (x >= maxX) {
            mLocked.pointerX = maxX;
        } else {
            mLocked.pointerX = x;
        }
        if (y <= minY) {
            mLocked.pointerY = minY;
        } else if (y >= maxY) {
            mLocked.pointerY = maxY;
        } else {
            mLocked.pointerY = y;
        }
        updatePointerLocked();
    }
}

void PointerController::getPosition(float* outX, float* outY) const {
    AutoMutex _l(mLock);

    *outX = mLocked.pointerX;
    *outY = mLocked.pointerY;
}

int32_t PointerController::getDisplayId() const {
    AutoMutex _l(mLock);

    return mLocked.viewport.displayId;
}

void PointerController::fade(Transition transition) {
    AutoMutex _l(mLock);

    // Remove the inactivity timeout, since we are fading now.
    removeInactivityTimeoutLocked();

    // Start fading.
    if (transition == Transition::IMMEDIATE) {
        mLocked.pointerFadeDirection = 0;
        mLocked.pointerAlpha = 0.0f;
        updatePointerLocked();
    } else {
        mLocked.pointerFadeDirection = -1;
        startAnimationLocked();
    }
}

void PointerController::unfade(Transition transition) {
    AutoMutex _l(mLock);

    // Always reset the inactivity timer.
    resetInactivityTimeoutLocked();

    // Start unfading.
    if (transition == Transition::IMMEDIATE) {
        mLocked.pointerFadeDirection = 0;
        mLocked.pointerAlpha = 1.0f;
        updatePointerLocked();
    } else {
        mLocked.pointerFadeDirection = 1;
        startAnimationLocked();
    }
}

void PointerController::setPresentation(Presentation presentation) {
    AutoMutex _l(mLock);

    if (mLocked.presentation == presentation) {
        return;
    }

    mLocked.presentation = presentation;
    mLocked.presentationChanged = true;

    if (!mLocked.viewport.isValid()) {
        return;
    }

    if (presentation == Presentation::POINTER) {
        if (mLocked.additionalMouseResources.empty()) {
            mPolicy->loadAdditionalMouseResources(&mLocked.additionalMouseResources,
                                                  &mLocked.animationResources,
                                                  mLocked.viewport.displayId);
        }
        fadeOutAndReleaseAllSpotsLocked();
        updatePointerLocked();
    }
}

void PointerController::setSpots(const PointerCoords* spotCoords,
        const uint32_t* spotIdToIndex, BitSet32 spotIdBits, int32_t displayId) {
#if DEBUG_POINTER_UPDATES
    ALOGD("setSpots: idBits=%08x", spotIdBits.value);
    for (BitSet32 idBits(spotIdBits); !idBits.isEmpty(); ) {
        uint32_t id = idBits.firstMarkedBit();
        idBits.clearBit(id);
        const PointerCoords& c = spotCoords[spotIdToIndex[id]];
        ALOGD(" spot %d: position=(%0.3f, %0.3f), pressure=%0.3f, displayId=%" PRId32 ".", id,
                c.getAxisValue(AMOTION_EVENT_AXIS_X),
                c.getAxisValue(AMOTION_EVENT_AXIS_Y),
                c.getAxisValue(AMOTION_EVENT_AXIS_PRESSURE),
                displayId);
    }
#endif

    AutoMutex _l(mLock);
    if (!mLocked.viewport.isValid()) {
        return;
    }

    std::vector<Spot*> newSpots;
    std::map<int32_t, std::vector<Spot*>>::const_iterator iter =
            mLocked.spotsByDisplay.find(displayId);
    if (iter != mLocked.spotsByDisplay.end()) {
        newSpots = iter->second;
    }

    mSpriteController->openTransaction();

    // Add or move spots for fingers that are down.
    for (BitSet32 idBits(spotIdBits); !idBits.isEmpty(); ) {
        uint32_t id = idBits.clearFirstMarkedBit();
        const PointerCoords& c = spotCoords[spotIdToIndex[id]];
        const SpriteIcon& icon = c.getAxisValue(AMOTION_EVENT_AXIS_PRESSURE) > 0
                ? mResources.spotTouch : mResources.spotHover;
        float x = c.getAxisValue(AMOTION_EVENT_AXIS_X);
        float y = c.getAxisValue(AMOTION_EVENT_AXIS_Y);

        Spot* spot = getSpot(id, newSpots);
        if (!spot) {
            spot = createAndAddSpotLocked(id, newSpots);
        }

        spot->updateSprite(&icon, x, y, displayId);
    }

    // Remove spots for fingers that went up.
    for (size_t i = 0; i < newSpots.size(); i++) {
        Spot* spot = newSpots[i];
        if (spot->id != Spot::INVALID_ID
                && !spotIdBits.hasBit(spot->id)) {
            fadeOutAndReleaseSpotLocked(spot);
        }
    }

    mSpriteController->closeTransaction();
    mLocked.spotsByDisplay[displayId] = newSpots;
}

void PointerController::clearSpots() {
#if DEBUG_POINTER_UPDATES
    ALOGD("clearSpots");
#endif

    AutoMutex _l(mLock);
    if (!mLocked.viewport.isValid()) {
        return;
    }

    fadeOutAndReleaseAllSpotsLocked();
}

void PointerController::setInactivityTimeout(InactivityTimeout inactivityTimeout) {
    AutoMutex _l(mLock);

    if (mLocked.inactivityTimeout != inactivityTimeout) {
        mLocked.inactivityTimeout = inactivityTimeout;
        resetInactivityTimeoutLocked();
    }
}

void PointerController::reloadPointerResources() {
    AutoMutex _l(mLock);

    loadResourcesLocked();
    updatePointerLocked();
}

/**
 * The viewport values for deviceHeight and deviceWidth have already been adjusted for rotation,
 * so here we are getting the dimensions in the original, unrotated orientation (orientation 0).
 */
static void getNonRotatedSize(const DisplayViewport& viewport, int32_t& width, int32_t& height) {
    width = viewport.deviceWidth;
    height = viewport.deviceHeight;

    if (viewport.orientation == DISPLAY_ORIENTATION_90
            || viewport.orientation == DISPLAY_ORIENTATION_270) {
        std::swap(width, height);
    }
}

void PointerController::setDisplayViewport(const DisplayViewport& viewport) {
    AutoMutex _l(mLock);
    if (viewport == mLocked.viewport) {
        return;
    }

    const DisplayViewport oldViewport = mLocked.viewport;
    mLocked.viewport = viewport;

    int32_t oldDisplayWidth, oldDisplayHeight;
    getNonRotatedSize(oldViewport, oldDisplayWidth, oldDisplayHeight);
    int32_t newDisplayWidth, newDisplayHeight;
    getNonRotatedSize(viewport, newDisplayWidth, newDisplayHeight);

    // Reset cursor position to center if size or display changed.
    if (oldViewport.displayId != viewport.displayId
            || oldDisplayWidth != newDisplayWidth
            || oldDisplayHeight != newDisplayHeight) {

        float minX, minY, maxX, maxY;
        if (getBoundsLocked(&minX, &minY, &maxX, &maxY)) {
            mLocked.pointerX = (minX + maxX) * 0.5f;
            mLocked.pointerY = (minY + maxY) * 0.5f;
            // Reload icon resources for density may be changed.
            loadResourcesLocked();
        } else {
            mLocked.pointerX = 0;
            mLocked.pointerY = 0;
        }

        fadeOutAndReleaseAllSpotsLocked();
    } else if (oldViewport.orientation != viewport.orientation) {
        // Apply offsets to convert from the pixel top-left corner position to the pixel center.
        // This creates an invariant frame of reference that we can easily rotate when
        // taking into account that the pointer may be located at fractional pixel offsets.
        float x = mLocked.pointerX + 0.5f;
        float y = mLocked.pointerY + 0.5f;
        float temp;

        // Undo the previous rotation.
        switch (oldViewport.orientation) {
        case DISPLAY_ORIENTATION_90:
            temp = x;
            x =  oldViewport.deviceHeight - y;
            y = temp;
            break;
        case DISPLAY_ORIENTATION_180:
            x = oldViewport.deviceWidth - x;
            y = oldViewport.deviceHeight - y;
            break;
        case DISPLAY_ORIENTATION_270:
            temp = x;
            x = y;
            y = oldViewport.deviceWidth - temp;
            break;
        }

        // Perform the new rotation.
        switch (viewport.orientation) {
        case DISPLAY_ORIENTATION_90:
            temp = x;
            x = y;
            y = viewport.deviceHeight - temp;
            break;
        case DISPLAY_ORIENTATION_180:
            x = viewport.deviceWidth - x;
            y = viewport.deviceHeight - y;
            break;
        case DISPLAY_ORIENTATION_270:
            temp = x;
            x = viewport.deviceWidth - y;
            y = temp;
            break;
        }

        // Apply offsets to convert from the pixel center to the pixel top-left corner position
        // and save the results.
        mLocked.pointerX = x - 0.5f;
        mLocked.pointerY = y - 0.5f;
    }

    updatePointerLocked();
}

void PointerController::updatePointerIcon(int32_t iconId) {
    AutoMutex _l(mLock);
    if (mLocked.requestedPointerType != iconId) {
        mLocked.requestedPointerType = iconId;
        mLocked.presentationChanged = true;
        updatePointerLocked();
    }
}

void PointerController::setCustomPointerIcon(const SpriteIcon& icon) {
    AutoMutex _l(mLock);

    const int32_t iconId = mPolicy->getCustomPointerIconId();
    mLocked.additionalMouseResources[iconId] = icon;
    mLocked.requestedPointerType = iconId;
    mLocked.presentationChanged = true;

    updatePointerLocked();
}

void PointerController::MessageHandler::handleMessage(const Message& message) {
    std::shared_ptr<PointerController> controller = pointerController.lock();

    if (controller == nullptr) {
        ALOGE("PointerController instance was released before processing message: what=%d",
              message.what);
        return;
    }
    switch (message.what) {
    case MSG_INACTIVITY_TIMEOUT:
        controller->doInactivityTimeout();
        break;
    }
}

int PointerController::LooperCallback::handleEvent(int /* fd */, int events, void* /* data */) {
    std::shared_ptr<PointerController> controller = pointerController.lock();
    if (controller == nullptr) {
        ALOGW("PointerController instance was released with pending callbacks.  events=0x%x",
              events);
        return 0; // Remove the callback, the PointerController is gone anyways
    }
    if (events & (Looper::EVENT_ERROR | Looper::EVENT_HANGUP)) {
        ALOGE("Display event receiver pipe was closed or an error occurred.  events=0x%x", events);
        return 0; // remove the callback
    }

    if (!(events & Looper::EVENT_INPUT)) {
        ALOGW("Received spurious callback for unhandled poll event.  events=0x%x", events);
        return 1; // keep the callback
    }

    bool gotVsync = false;
    ssize_t n;
    nsecs_t timestamp;
    DisplayEventReceiver::Event buf[EVENT_BUFFER_SIZE];
    while ((n = controller->mDisplayEventReceiver.getEvents(buf, EVENT_BUFFER_SIZE)) > 0) {
        for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
            if (buf[i].header.type == DisplayEventReceiver::DISPLAY_EVENT_VSYNC) {
                timestamp = buf[i].header.timestamp;
                gotVsync = true;
            }
        }
    }
    if (gotVsync) {
        controller->doAnimate(timestamp);
    }
    return 1;  // keep the callback
}

void PointerController::doAnimate(nsecs_t timestamp) {
    AutoMutex _l(mLock);

    mLocked.animationPending = false;

    bool keepFading = doFadingAnimationLocked(timestamp);
    bool keepBitmapFlipping = doBitmapAnimationLocked(timestamp);
    if (keepFading || keepBitmapFlipping) {
        startAnimationLocked();
    }
}

bool PointerController::doFadingAnimationLocked(nsecs_t timestamp) {
    bool keepAnimating = false;
    nsecs_t frameDelay = timestamp - mLocked.animationTime;

    // Animate pointer fade.
    if (mLocked.pointerFadeDirection < 0) {
        mLocked.pointerAlpha -= float(frameDelay) / POINTER_FADE_DURATION;
        if (mLocked.pointerAlpha <= 0.0f) {
            mLocked.pointerAlpha = 0.0f;
            mLocked.pointerFadeDirection = 0;
        } else {
            keepAnimating = true;
        }
        updatePointerLocked();
    } else if (mLocked.pointerFadeDirection > 0) {
        mLocked.pointerAlpha += float(frameDelay) / POINTER_FADE_DURATION;
        if (mLocked.pointerAlpha >= 1.0f) {
            mLocked.pointerAlpha = 1.0f;
            mLocked.pointerFadeDirection = 0;
        } else {
            keepAnimating = true;
        }
        updatePointerLocked();
    }

    // Animate spots that are fading out and being removed.
    for(auto it = mLocked.spotsByDisplay.begin(); it != mLocked.spotsByDisplay.end();) {
        std::vector<Spot*>& spots = it->second;
        size_t numSpots = spots.size();
        for (size_t i = 0; i < numSpots;) {
            Spot* spot = spots[i];
            if (spot->id == Spot::INVALID_ID) {
                spot->alpha -= float(frameDelay) / SPOT_FADE_DURATION;
                if (spot->alpha <= 0) {
                    spots.erase(spots.begin() + i);
                    releaseSpotLocked(spot);
                    numSpots--;
                    continue;
                } else {
                    spot->sprite->setAlpha(spot->alpha);
                    keepAnimating = true;
                }
            }
            ++i;
        }

        if (spots.size() == 0) {
            it = mLocked.spotsByDisplay.erase(it);
        } else {
            ++it;
        }
    }

    return keepAnimating;
}

bool PointerController::doBitmapAnimationLocked(nsecs_t timestamp) {
    std::map<int32_t, PointerAnimation>::const_iterator iter = mLocked.animationResources.find(
            mLocked.requestedPointerType);
    if (iter == mLocked.animationResources.end()) {
        return false;
    }

    if (timestamp - mLocked.lastFrameUpdatedTime > iter->second.durationPerFrame) {
        mSpriteController->openTransaction();

        int incr = (timestamp - mLocked.lastFrameUpdatedTime) / iter->second.durationPerFrame;
        mLocked.animationFrameIndex += incr;
        mLocked.lastFrameUpdatedTime += iter->second.durationPerFrame * incr;
        while (mLocked.animationFrameIndex >= iter->second.animationFrames.size()) {
            mLocked.animationFrameIndex -= iter->second.animationFrames.size();
        }
        mLocked.pointerSprite->setIcon(iter->second.animationFrames[mLocked.animationFrameIndex]);

        mSpriteController->closeTransaction();
    }

    // Keep animating.
    return true;
}

void PointerController::doInactivityTimeout() {
    fade(Transition::GRADUAL);
}

void PointerController::startAnimationLocked() {
    if (!mLocked.animationPending) {
        mLocked.animationPending = true;
        mLocked.animationTime = systemTime(SYSTEM_TIME_MONOTONIC);
        mDisplayEventReceiver.requestNextVsync();
    }
}

void PointerController::resetInactivityTimeoutLocked() {
    mLooper->removeMessages(mHandler, MSG_INACTIVITY_TIMEOUT);

    nsecs_t timeout = mLocked.inactivityTimeout == InactivityTimeout::SHORT
            ? INACTIVITY_TIMEOUT_DELAY_TIME_SHORT
            : INACTIVITY_TIMEOUT_DELAY_TIME_NORMAL;
    mLooper->sendMessageDelayed(timeout, mHandler, MSG_INACTIVITY_TIMEOUT);
}

void PointerController::removeInactivityTimeoutLocked() {
    mLooper->removeMessages(mHandler, MSG_INACTIVITY_TIMEOUT);
}

void PointerController::updatePointerLocked() REQUIRES(mLock) {
    if (!mLocked.viewport.isValid()) {
        return;
    }

    mSpriteController->openTransaction();

    mLocked.pointerSprite->setLayer(Sprite::BASE_LAYER_POINTER);
    mLocked.pointerSprite->setPosition(mLocked.pointerX, mLocked.pointerY);
    mLocked.pointerSprite->setDisplayId(mLocked.viewport.displayId);

    if (mLocked.pointerAlpha > 0) {
        mLocked.pointerSprite->setAlpha(mLocked.pointerAlpha);
        mLocked.pointerSprite->setVisible(true);
    } else {
        mLocked.pointerSprite->setVisible(false);
    }

    if (mLocked.pointerIconChanged || mLocked.presentationChanged) {
        if (mLocked.presentation == Presentation::POINTER) {
            if (mLocked.requestedPointerType == mPolicy->getDefaultPointerIconId()) {
                mLocked.pointerSprite->setIcon(mLocked.pointerIcon);
            } else {
                std::map<int32_t, SpriteIcon>::const_iterator iter =
                    mLocked.additionalMouseResources.find(mLocked.requestedPointerType);
                if (iter != mLocked.additionalMouseResources.end()) {
                    std::map<int32_t, PointerAnimation>::const_iterator anim_iter =
                            mLocked.animationResources.find(mLocked.requestedPointerType);
                    if (anim_iter != mLocked.animationResources.end()) {
                        mLocked.animationFrameIndex = 0;
                        mLocked.lastFrameUpdatedTime = systemTime(SYSTEM_TIME_MONOTONIC);
                        startAnimationLocked();
                    }
                    mLocked.pointerSprite->setIcon(iter->second);
                } else {
                    ALOGW("Can't find the resource for icon id %d", mLocked.requestedPointerType);
                    mLocked.pointerSprite->setIcon(mLocked.pointerIcon);
                }
            }
        } else {
            mLocked.pointerSprite->setIcon(mResources.spotAnchor);
        }
        mLocked.pointerIconChanged = false;
        mLocked.presentationChanged = false;
    }

    mSpriteController->closeTransaction();
}

PointerController::Spot* PointerController::getSpot(uint32_t id, const std::vector<Spot*>& spots) {
    for (size_t i = 0; i < spots.size(); i++) {
        Spot* spot = spots[i];
        if (spot->id == id) {
            return spot;
        }
    }

    return nullptr;
}

PointerController::Spot* PointerController::createAndAddSpotLocked(uint32_t id,
        std::vector<Spot*>& spots) {
    // Remove spots until we have fewer than MAX_SPOTS remaining.
    while (spots.size() >= MAX_SPOTS) {
        Spot* spot = removeFirstFadingSpotLocked(spots);
        if (!spot) {
            spot = spots[0];
            spots.erase(spots.begin());
        }
        releaseSpotLocked(spot);
    }

    // Obtain a sprite from the recycled pool.
    sp<Sprite> sprite;
    if (! mLocked.recycledSprites.empty()) {
        sprite = mLocked.recycledSprites.back();
        mLocked.recycledSprites.pop_back();
    } else {
        sprite = mSpriteController->createSprite();
    }

    // Return the new spot.
    Spot* spot = new Spot(id, sprite);
    spots.push_back(spot);
    return spot;
}

PointerController::Spot* PointerController::removeFirstFadingSpotLocked(std::vector<Spot*>& spots) {
    for (size_t i = 0; i < spots.size(); i++) {
        Spot* spot = spots[i];
        if (spot->id == Spot::INVALID_ID) {
            spots.erase(spots.begin() + i);
            return spot;
        }
    }
    return nullptr;
}

void PointerController::releaseSpotLocked(Spot* spot) {
    spot->sprite->clearIcon();

    if (mLocked.recycledSprites.size() < MAX_RECYCLED_SPRITES) {
        mLocked.recycledSprites.push_back(spot->sprite);
    }

    delete spot;
}

void PointerController::fadeOutAndReleaseSpotLocked(Spot* spot) {
    if (spot->id != Spot::INVALID_ID) {
        spot->id = Spot::INVALID_ID;
        startAnimationLocked();
    }
}

void PointerController::fadeOutAndReleaseAllSpotsLocked() {
    for (auto& it : mLocked.spotsByDisplay) {
        const std::vector<Spot*>& spots = it.second;
        size_t numSpots = spots.size();
        for (size_t i = 0; i < numSpots; i++) {
            Spot* spot = spots[i];
            fadeOutAndReleaseSpotLocked(spot);
        }
    }
}

void PointerController::loadResourcesLocked() REQUIRES(mLock) {
    if (!mLocked.viewport.isValid()) {
        return;
    }

    mPolicy->loadPointerResources(&mResources, mLocked.viewport.displayId);
    mPolicy->loadPointerIcon(&mLocked.pointerIcon, mLocked.viewport.displayId);

    mLocked.additionalMouseResources.clear();
    mLocked.animationResources.clear();
    if (mLocked.presentation == Presentation::POINTER) {
        mPolicy->loadAdditionalMouseResources(&mLocked.additionalMouseResources,
                &mLocked.animationResources, mLocked.viewport.displayId);
    }

    mLocked.pointerIconChanged = true;
}


// --- PointerController::Spot ---

void PointerController::Spot::updateSprite(const SpriteIcon* icon, float x, float y,
        int32_t displayId) {
    sprite->setLayer(Sprite::BASE_LAYER_SPOT + id);
    sprite->setAlpha(alpha);
    sprite->setTransformationMatrix(SpriteTransformationMatrix(scale, 0.0f, 0.0f, scale));
    sprite->setPosition(x, y);
    sprite->setDisplayId(displayId);
    this->x = x;
    this->y = y;

    if (icon != lastIcon) {
        lastIcon = icon;
        if (icon) {
            sprite->setIcon(*icon);
            sprite->setVisible(true);
        } else {
            sprite->setVisible(false);
        }
    }
}

} // namespace android
