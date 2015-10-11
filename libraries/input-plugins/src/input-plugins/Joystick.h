//
//  Joystick.h
//  input-plugins/src/input-plugins
//
//  Created by Stephen Birarda on 2014-09-23.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_Joystick_h
#define hifi_Joystick_h

#include <qobject.h>
#include <qvector.h>

#ifdef HAVE_SDL2
#include <SDL.h>
#undef main
#endif

#include "InputDevice.h"
#include "StandardControls.h"

class Joystick : public QObject, public InputDevice {
    Q_OBJECT
    Q_PROPERTY(QString name READ getName)

#ifdef HAVE_SDL2
    Q_PROPERTY(int instanceId READ getInstanceId)
#endif
    
public:

    const QString& getName() const { return _name; }

    // Device functions
    virtual void registerToUserInputMapper(UserInputMapper& mapper) override;
    virtual void assignDefaultInputMapping(UserInputMapper& mapper) override;
    virtual void update(float deltaTime, bool jointsCaptured) override;
    virtual void focusOutEvent() override;
    
    Joystick() : InputDevice("Joystick") {}
    ~Joystick();
    
    UserInputMapper::Input makeInput(Controllers::StandardButtonChannel button);
    UserInputMapper::Input makeInput(Controllers::StandardAxisChannel axis);
    
#ifdef HAVE_SDL2
    Joystick(SDL_JoystickID instanceId, const QString& name, SDL_GameController* sdlGameController);
#endif
    
    void closeJoystick();

#ifdef HAVE_SDL2
    void handleAxisEvent(const SDL_ControllerAxisEvent& event);
    void handleButtonEvent(const SDL_ControllerButtonEvent& event);
#endif
    
#ifdef HAVE_SDL2
    int getInstanceId() const { return _instanceId; }
#endif
    
private:
#ifdef HAVE_SDL2
    SDL_GameController* _sdlGameController;
    SDL_Joystick* _sdlJoystick;
    SDL_JoystickID _instanceId;
#endif
};

#endif // hifi_Joystick_h
