#pragma once

/**
 * MCMVRInput - VR-native controller input for MCM
 * 
 * Directly polls OpenVR for controller state, bypassing the game's
 * input translation layer which doesn't correctly map all VR buttons.
 * 
 * No dependencies on other mods required - only uses OpenVR API.
 */

#include <cstdint>

namespace MCMVRInput
{
    // VR Button IDs (from OpenVR EVRButtonId)
    enum VRButton : uint32_t
    {
        System = 0,
        ApplicationMenu = 1,  // B/Y button
        Grip = 2,             // Grip/A button on Index
        DPad_Left = 3,
        DPad_Up = 4,
        DPad_Right = 5,
        DPad_Down = 6,
        A = 7,                // A/X button
        Thumbstick = 32,      // k_EButton_Axis0 - thumbstick click
        Trigger = 33,         // k_EButton_Axis1
    };

    // MCM Control actions
    enum MCMAction
    {
        None = 0,
        Up,
        Down,
        Left,
        Right,
        Accept,     // A button or Trigger
        Cancel,     // B button
        TabLeft,    // Left shoulder/grip
        TabRight,   // Right shoulder/grip
    };

    /**
     * Initialize the VR input system
     * Call once during plugin load
     */
    bool Initialize();

    /**
     * Update and process VR controller input
     * Call this each frame when MCM menu is open
     * Returns true if any input was processed
     */
    bool Update();

    /**
     * Check if a specific action was just pressed this frame
     */
    bool WasActionPressed(MCMAction action);

    /**
     * Check if a specific action is currently held
     */
    bool IsActionHeld(MCMAction action);

    /**
     * Get thumbstick X axis (-1.0 to 1.0)
     */
    float GetThumbstickX();

    /**
     * Get thumbstick Y axis (-1.0 to 1.0)
     */
    float GetThumbstickY();

    /**
     * Shutdown and cleanup
     */
    void Shutdown();
}
