/**
 * MCMVRInput - VR-native controller input for MCM
 * 
 * Directly polls OpenVR for controller state, bypassing the game's
 * input translation layer which doesn't correctly map all VR buttons.
 */

#include "MCMVRInput.h"
#include "ScaleformMCM.h"
#include "f4se/GameInput.h"
#include "f4se/InputMap.h"

//==============================================================================
// OpenVR Types and Constants (local definitions to avoid header dependency)
//==============================================================================

typedef uint32_t TrackedDeviceIndex_t;
const TrackedDeviceIndex_t k_unTrackedDeviceIndexInvalid = 0xFFFFFFFF;

// OpenVR button IDs
enum EVRButtonId
{
    k_EButton_System = 0,
    k_EButton_ApplicationMenu = 1,  // B/Y button on Oculus Touch
    k_EButton_Grip = 2,
    k_EButton_DPad_Left = 3,
    k_EButton_DPad_Up = 4,
    k_EButton_DPad_Right = 5,
    k_EButton_DPad_Down = 6,
    k_EButton_A = 7,                // A/X button on Oculus Touch
    k_EButton_Axis0 = 32,           // Thumbstick/Touchpad
    k_EButton_Axis1 = 33,           // Trigger
};

enum ETrackedControllerRole
{
    TrackedControllerRole_Invalid = 0,
    TrackedControllerRole_LeftHand = 1,
    TrackedControllerRole_RightHand = 2,
};

// VRControllerState - Natural alignment on Windows (pack(4) only on Linux/Apple)
struct VRControllerAxis_t
{
    float x;
    float y;
};

struct VRControllerState001_t
{
    uint32_t unPacketNum;
    uint64_t ulButtonPressed;
    uint64_t ulButtonTouched;
    VRControllerAxis_t rAxis[5];
};

// Function pointer types for the OpenVR C API
typedef void* (*VR_GetGenericInterfaceFn)(const char* pchInterfaceVersion, int* peError);

//==============================================================================
// IVRSystem vtable helpers
// 
// IVRSystem is a C++ class with vtable. The method indices are stable within
// a major OpenVR version. We call through function pointers derived from vtable.
//==============================================================================

// Get vtable pointer from IVRSystem instance
inline void** GetVTable(void* instance)
{
    return *reinterpret_cast<void***>(instance);
}

// IVRSystem vtable indices (OpenVR 1.x - stable)
// From openvr.h IVRSystem class definition order
// These indices were verified against Heisenberg's OpenVRHook.h
const int VTABLE_GetTrackedDeviceIndexForControllerRole = 18;
const int VTABLE_GetControllerState = 34;

// Function types matching vtable methods
// NOTE: On x64 Windows, there is no __thiscall - the 'this' pointer is passed in RCX
// and subsequent params in RDX, R8, R9. We just use standard calling convention.
typedef TrackedDeviceIndex_t(*GetTrackedDeviceIndexForControllerRoleFn)(void* thisptr, ETrackedControllerRole role);
typedef bool(*GetControllerStateFn)(void* thisptr, TrackedDeviceIndex_t index, VRControllerState001_t* pState, uint32_t unStateSize);

//==============================================================================
// Module State
//==============================================================================

namespace MCMVRInput
{
    // OpenVR interface
    static HMODULE g_openvrDll = nullptr;
    static void* g_vrSystem = nullptr;
    static GetTrackedDeviceIndexForControllerRoleFn g_fnGetControllerIndex = nullptr;
    static GetControllerStateFn g_fnGetControllerState = nullptr;

    static bool g_initialized = false;

    // Controller state
    static VRControllerState001_t g_rightState = {};
    static VRControllerState001_t g_rightStatePrev = {};
    static VRControllerState001_t g_leftState = {};
    static VRControllerState001_t g_leftStatePrev = {};

    // Thumbstick navigation state
    static int g_currentThumbstickDirection = 0;  // 0=none, 1=up, 2=right, 3=down, 4=left
    static int g_lastThumbstickDirection = 0;
    static float g_thumbstickRepeatTimer = 0.0f;
    static const float THUMBSTICK_THRESHOLD = 0.5f;
    static const float THUMBSTICK_INITIAL_DELAY = 0.4f;
    static const float THUMBSTICK_REPEAT_DELAY = 0.15f;

    // Action state (set each frame based on button changes)
    static uint32_t g_actionsPressed = 0;
    static uint32_t g_actionsHeld = 0;

    // Thumbstick values for external access
    static float g_thumbstickX = 0.0f;
    static float g_thumbstickY = 0.0f;

    //==========================================================================
    // Helper Functions
    //==========================================================================

    // Check if button was just pressed
    inline bool ButtonJustPressed(uint64_t current, uint64_t previous, EVRButtonId button)
    {
        uint64_t mask = 1ull << button;
        return (current & mask) && !(previous & mask);
    }

    // Check if button is held
    inline bool ButtonHeld(uint64_t current, EVRButtonId button)
    {
        return (current & (1ull << button)) != 0;
    }

    // Get thumbstick direction from axis values
    int GetThumbstickDirection(float x, float y)
    {
        if (y > THUMBSTICK_THRESHOLD) return 1;  // Up
        if (x > THUMBSTICK_THRESHOLD) return 2;  // Right
        if (y < -THUMBSTICK_THRESHOLD) return 3; // Down
        if (x < -THUMBSTICK_THRESHOLD) return 4; // Left
        return 0; // None
    }

    //==========================================================================
    // Public API
    //==========================================================================

    bool Initialize()
    {
        if (g_initialized) return true;

        // Get the already-loaded OpenVR DLL
        g_openvrDll = GetModuleHandleA("openvr_api.dll");
        if (!g_openvrDll)
        {
            _MESSAGE("MCMVRInput: openvr_api.dll not loaded - VR not active?");
            return false;
        }

        // Get VR_GetGenericInterface - the main entry point for OpenVR interfaces
        auto VR_GetGenericInterface = (VR_GetGenericInterfaceFn)GetProcAddress(g_openvrDll, "VR_GetGenericInterface");
        if (!VR_GetGenericInterface)
        {
            _MESSAGE("MCMVRInput: VR_GetGenericInterface not found");
            return false;
        }

        // Request IVRSystem interface
        // Version string must match what F4VR is using (usually IVRSystem_022 or similar)
        // We try multiple versions for compatibility
        const char* versions[] = {
            "IVRSystem_022",
            "IVRSystem_021",
            "IVRSystem_020",
            "IVRSystem_019",
            nullptr
        };

        int error = 0;
        for (int i = 0; versions[i]; i++)
        {
            g_vrSystem = VR_GetGenericInterface(versions[i], &error);
            if (g_vrSystem)
            {
                _MESSAGE("MCMVRInput: Got %s interface", versions[i]);
                break;
            }
        }

        if (!g_vrSystem)
        {
            _MESSAGE("MCMVRInput: Could not get IVRSystem interface (error %d)", error);
            return false;
        }

        // Extract function pointers from vtable
        void** vtable = GetVTable(g_vrSystem);
        g_fnGetControllerIndex = (GetTrackedDeviceIndexForControllerRoleFn)vtable[VTABLE_GetTrackedDeviceIndexForControllerRole];
        g_fnGetControllerState = (GetControllerStateFn)vtable[VTABLE_GetControllerState];

        if (!g_fnGetControllerIndex || !g_fnGetControllerState)
        {
            _MESSAGE("MCMVRInput: Failed to get vtable function pointers");
            return false;
        }

        g_initialized = true;
        _MESSAGE("MCMVRInput: Initialized successfully");
        return true;
    }

    static int g_updateCallCount = 0;

    bool Update()
    {
        if (!g_initialized) return false;

        // Debug: Log that Update is being called (first 5 calls only)
        g_updateCallCount++;
        if (g_updateCallCount <= 5)
        {
            _MESSAGE("MCMVRInput: Update() called (count=%d)", g_updateCallCount);
        }

        // Save previous state
        g_rightStatePrev = g_rightState;
        g_leftStatePrev = g_leftState;

        // Get controller indices
        TrackedDeviceIndex_t rightIndex = g_fnGetControllerIndex(g_vrSystem, TrackedControllerRole_RightHand);
        TrackedDeviceIndex_t leftIndex = g_fnGetControllerIndex(g_vrSystem, TrackedControllerRole_LeftHand);

        // Debug: Log controller indices once
        static bool loggedIndices = false;
        if (!loggedIndices)
        {
            _MESSAGE("MCMVRInput: Controller indices - Right=%u, Left=%u (invalid=%u)", rightIndex, leftIndex, k_unTrackedDeviceIndexInvalid);
            loggedIndices = true;
        }

        // Get controller states
        static int getStateLogCount = 0;
        bool rightSuccess = false, leftSuccess = false;
        if (rightIndex != k_unTrackedDeviceIndexInvalid)
        {
            rightSuccess = g_fnGetControllerState(g_vrSystem, rightIndex, &g_rightState, sizeof(g_rightState));
        }
        if (leftIndex != k_unTrackedDeviceIndexInvalid)
        {
            leftSuccess = g_fnGetControllerState(g_vrSystem, leftIndex, &g_leftState, sizeof(g_leftState));
        }

        // Debug: Log GetControllerState results once
        if (getStateLogCount < 3)
        {
            _MESSAGE("MCMVRInput: GetControllerState - rightOK=%d leftOK=%d packetR=%u packetL=%u structSize=%zu", 
                rightSuccess, leftSuccess, g_rightState.unPacketNum, g_leftState.unPacketNum, sizeof(g_rightState));
            getStateLogCount++;
        }

        // Debug: Log ANY non-zero button state (first 20 times we see buttons pressed)
        static int buttonLogCount = 0;
        if (buttonLogCount < 20)
        {
            if (g_rightState.ulButtonPressed != 0 || g_leftState.ulButtonPressed != 0)
            {
                _MESSAGE("MCMVRInput: Buttons - Right=0x%llX, Left=0x%llX", g_rightState.ulButtonPressed, g_leftState.ulButtonPressed);
                buttonLogCount++;
            }
        }

        // Debug: Log button state changes
        if (g_rightState.ulButtonPressed != g_rightStatePrev.ulButtonPressed)
        {
            _MESSAGE("MCMVRInput: RIGHT buttons changed: 0x%llX -> 0x%llX", g_rightStatePrev.ulButtonPressed, g_rightState.ulButtonPressed);
        }
        if (g_leftState.ulButtonPressed != g_leftStatePrev.ulButtonPressed)
        {
            _MESSAGE("MCMVRInput: LEFT buttons changed: 0x%llX -> 0x%llX", g_leftStatePrev.ulButtonPressed, g_leftState.ulButtonPressed);
        }

        // Store thumbstick values (right controller, axis 0 is thumbstick/touchpad)
        g_thumbstickX = g_rightState.rAxis[0].x;
        g_thumbstickY = g_rightState.rAxis[0].y;

        // Reset action flags
        g_actionsPressed = 0;
        g_actionsHeld = 0;

        bool inputProcessed = false;

        // =====================================================================
        // BUTTON MAPPINGS
        // =====================================================================

        // Check individual button presses for debugging
        bool rightA = ButtonJustPressed(g_rightState.ulButtonPressed, g_rightStatePrev.ulButtonPressed, k_EButton_A);
        bool rightTrigger = ButtonJustPressed(g_rightState.ulButtonPressed, g_rightStatePrev.ulButtonPressed, k_EButton_Axis1);
        bool leftA = ButtonJustPressed(g_leftState.ulButtonPressed, g_leftStatePrev.ulButtonPressed, k_EButton_A);
        bool leftTrigger = ButtonJustPressed(g_leftState.ulButtonPressed, g_leftStatePrev.ulButtonPressed, k_EButton_Axis1);

        // Accept: A button (either) or Trigger (either)
        if (rightA || rightTrigger || leftA || leftTrigger)
        {
            g_actionsPressed |= (1 << Accept);
            ScaleformMCM::ProcessUserEvent("Accept", true, InputEvent::kDeviceType_Gamepad);
            ScaleformMCM::ProcessKeyEvent(InputMap::kGamepadButtonOffset_A, true);
            _MESSAGE("MCMVRInput: Accept pressed - rightA=%d rightTrig=%d leftA=%d leftTrig=%d", rightA, rightTrigger, leftA, leftTrigger);
            inputProcessed = true;
        }

        // Check grip and B buttons for Cancel
        bool rightB = ButtonJustPressed(g_rightState.ulButtonPressed, g_rightStatePrev.ulButtonPressed, k_EButton_ApplicationMenu);
        bool leftB = ButtonJustPressed(g_leftState.ulButtonPressed, g_leftStatePrev.ulButtonPressed, k_EButton_ApplicationMenu);
        bool rightGrip = ButtonJustPressed(g_rightState.ulButtonPressed, g_rightStatePrev.ulButtonPressed, k_EButton_Grip);
        bool leftGrip = ButtonJustPressed(g_leftState.ulButtonPressed, g_leftStatePrev.ulButtonPressed, k_EButton_Grip);

        // Cancel/Back: B button (either) OR Grip (either) - Grip goes back in menu
        if (rightB || leftB || rightGrip || leftGrip)
        {
            g_actionsPressed |= (1 << Cancel);
            ScaleformMCM::ProcessUserEvent("Cancel", true, InputEvent::kDeviceType_Gamepad);
            ScaleformMCM::ProcessKeyEvent(InputMap::kGamepadButtonOffset_B, true);
            _MESSAGE("MCMVRInput: Cancel/Back pressed - rightB=%d leftB=%d rightGrip=%d leftGrip=%d", rightB, leftB, rightGrip, leftGrip);
            inputProcessed = true;
        }

        // =====================================================================
        // THUMBSTICK NAVIGATION - DISABLED
        // Thumbstick navigation is now handled directly in OnThumbstickEvent
        // using NavigateList() for more reliable VR input.
        // Keeping this code would cause double-navigation.
        // =====================================================================
#if 0
        g_lastThumbstickDirection = g_currentThumbstickDirection;
        g_currentThumbstickDirection = GetThumbstickDirection(g_thumbstickX, g_thumbstickY);

        // Direction changed
        if (g_currentThumbstickDirection != g_lastThumbstickDirection)
        {
            // Release previous direction
            if (g_lastThumbstickDirection != 0)
            {
                const char* releaseControl = nullptr;
                UInt32 releaseKey = 0;
                switch (g_lastThumbstickDirection)
                {
                    case 1: releaseControl = "Up"; releaseKey = InputMap::kGamepadButtonOffset_DPAD_UP; break;
                    case 2: releaseControl = "Right"; releaseKey = InputMap::kGamepadButtonOffset_DPAD_RIGHT; break;
                    case 3: releaseControl = "Down"; releaseKey = InputMap::kGamepadButtonOffset_DPAD_DOWN; break;
                    case 4: releaseControl = "Left"; releaseKey = InputMap::kGamepadButtonOffset_DPAD_LEFT; break;
                }
                if (releaseControl)
                {
                    ScaleformMCM::ProcessUserEvent(releaseControl, false, InputEvent::kDeviceType_Gamepad);
                    ScaleformMCM::ProcessKeyEvent(releaseKey, false);
                }
            }

            // Press new direction
            if (g_currentThumbstickDirection != 0)
            {
                const char* pressControl = nullptr;
                UInt32 pressKey = 0;
                MCMAction action = None;
                switch (g_currentThumbstickDirection)
                {
                    case 1: pressControl = "Up"; pressKey = InputMap::kGamepadButtonOffset_DPAD_UP; action = Up; break;
                    case 2: pressControl = "Right"; pressKey = InputMap::kGamepadButtonOffset_DPAD_RIGHT; action = Right; break;
                    case 3: pressControl = "Down"; pressKey = InputMap::kGamepadButtonOffset_DPAD_DOWN; action = Down; break;
                    case 4: pressControl = "Left"; pressKey = InputMap::kGamepadButtonOffset_DPAD_LEFT; action = Left; break;
                }
                if (pressControl)
                {
                    g_actionsPressed |= (1 << action);
                    ScaleformMCM::ProcessUserEvent(pressControl, true, InputEvent::kDeviceType_Gamepad);
                    ScaleformMCM::ProcessKeyEvent(pressKey, true);
                    inputProcessed = true;
                }

                // Reset repeat timer
                g_thumbstickRepeatTimer = THUMBSTICK_INITIAL_DELAY;
            }
        }
        else if (g_currentThumbstickDirection != 0)
        {
            // Same direction held - handle repeat
            g_thumbstickRepeatTimer -= 0.016f;  // Assuming ~60fps
            if (g_thumbstickRepeatTimer <= 0.0f)
            {
                // Trigger repeat
                const char* repeatControl = nullptr;
                UInt32 repeatKey = 0;
                MCMAction action = None;
                switch (g_currentThumbstickDirection)
                {
                    case 1: repeatControl = "Up"; repeatKey = InputMap::kGamepadButtonOffset_DPAD_UP; action = Up; break;
                    case 2: repeatControl = "Right"; repeatKey = InputMap::kGamepadButtonOffset_DPAD_RIGHT; action = Right; break;
                    case 3: repeatControl = "Down"; repeatKey = InputMap::kGamepadButtonOffset_DPAD_DOWN; action = Down; break;
                    case 4: repeatControl = "Left"; repeatKey = InputMap::kGamepadButtonOffset_DPAD_LEFT; action = Left; break;
                }
                if (repeatControl)
                {
                    g_actionsPressed |= (1 << action);
                    ScaleformMCM::ProcessUserEvent(repeatControl, true, InputEvent::kDeviceType_Gamepad);
                    ScaleformMCM::ProcessKeyEvent(repeatKey, true);
                    inputProcessed = true;
                }
                g_thumbstickRepeatTimer = THUMBSTICK_REPEAT_DELAY;
            }
        }
#endif  // Disabled thumbstick navigation - now handled in OnThumbstickEvent

        // Update held state
        if (ButtonHeld(g_rightState.ulButtonPressed, k_EButton_A) || 
            ButtonHeld(g_rightState.ulButtonPressed, k_EButton_Axis1))
            g_actionsHeld |= (1 << Accept);
        if (ButtonHeld(g_rightState.ulButtonPressed, k_EButton_ApplicationMenu))
            g_actionsHeld |= (1 << Cancel);

        return inputProcessed;
    }

    bool WasActionPressed(MCMAction action)
    {
        return (g_actionsPressed & (1 << action)) != 0;
    }

    bool IsActionHeld(MCMAction action)
    {
        return (g_actionsHeld & (1 << action)) != 0;
    }

    float GetThumbstickX()
    {
        return g_thumbstickX;
    }

    float GetThumbstickY()
    {
        return g_thumbstickY;
    }

    void Shutdown()
    {
        g_vrSystem = nullptr;
        g_fnGetControllerIndex = nullptr;
        g_fnGetControllerState = nullptr;
        g_initialized = false;
        _MESSAGE("MCMVRInput: Shutdown");
    }
}
