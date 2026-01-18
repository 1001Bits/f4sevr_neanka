#include "ScaleformMCM.h"

#include "f4se/ScaleformValue.h"
#include "f4se/ScaleformMovie.h"
#include "f4se/ScaleformCallbacks.h"
#include "f4se/PapyrusScaleformAdapter.h"

#include "f4se/PapyrusEvents.h"
#include "f4se/PapyrusUtilities.h"

#include "f4se/GameData.h"
#include "f4se/GameRTTI.h"
#include "f4se/GameMenus.h"
#include "f4se/GameInput.h"
#include "f4se/InputMap.h"

#include "Globals.h"
#include "Config.h"
#include "Utils.h"
#include "SettingStore.h"
#include "MCMKeybinds.h"

// VR-native controller input
#include "MCMVRInput.h"

#include <chrono>

// Debounce timer for GoBackOneMenu to prevent double-trigger
static std::chrono::steady_clock::time_point g_lastGoBackTime;
static bool g_goBackTimeInitialized = false;

namespace ScaleformMCM {

	// function GetMCMVersionString():String;
	class GetMCMVersionString : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetString(PLUGIN_VERSION_STRING);
		}
	};

	// function GetMCMVersionCode():int;
	class GetMCMVersionCode : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetInt(PLUGIN_VERSION);
		}
	};

	// function GetConfigList(fullPath:Boolean=false, filename:String="config.json"):Array;
	// Returns: ["Mod1", "Mod2", "Mod3"] (fullPath = false), or ["Data\MCM\Config\Mod1\config.json", ...] (fullPath = true)
	class GetConfigList : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			bool wantFullPath		= (args->numArgs > 0 && args->args[0].GetType() == GFxValue::kType_Bool) ? args->args[0].GetBool() : false;
			const char* filename	= (args->numArgs > 1 && args->args[1].GetType() == GFxValue::kType_String) ? args->args[1].GetString() : "config.json";

			args->movie->movieRoot->CreateArray(args->result);

			HANDLE hFind;
			WIN32_FIND_DATA data;

			hFind = FindFirstFile("Data\\MCM\\Config\\*", &data);
			if (hFind != INVALID_HANDLE_VALUE) {
				do {
					if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
					if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, "..")) continue;

					char fullPath[MAX_PATH];
					snprintf(fullPath, MAX_PATH, "%s%s%s%s", "Data\\MCM\\Config\\", data.cFileName, "\\", filename);

					if (GetFileAttributes(fullPath) == INVALID_FILE_ATTRIBUTES) continue;

					GFxValue filePath;
					filePath.SetString(wantFullPath ? fullPath : data.cFileName);
					args->result->PushBack(&filePath);
				} while (FindNextFile(hFind, &data));
				FindClose(hFind);
			}
		}
	};

	// function OnMCMOpen();
	class OnMCMOpen : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			// Start key handler
			RegisterForInput(true);
			// Initialize VR-native input
			MCMVRInput::Initialize();
		}
	};

	// function OnMCMClose();
	class OnMCMClose : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			// Save modified keybinds.
			g_keybindManager.CommitKeybinds();
			RegisterForInput(false);
		}
	};

	// function DisableMenuInput(disable:Boolean);
	class DisableMenuInput : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			if (args->numArgs < 1) return;
			if (args->args[0].GetType() != GFxValue::kType_Bool) return;
			bool disable = args->args[0].GetBool();

			MCMUtils::DisableProcessUserEvent(disable);
		}
	};

	// function GetGlobalValue(formIdentifier:String):Number
	class GetGlobalValue : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetNumber(-1);

			if (args->numArgs != 1) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;

			TESForm* targetForm = MCMUtils::GetFormFromIdentifier(args->args[0].GetString());
			TESGlobal* global = nullptr;
			global = DYNAMIC_CAST(targetForm, TESForm, TESGlobal);

			if (global) {
				args->result->SetNumber(global->value);
			}
		}
	};

	// function SetGlobalValue(formIdentifier:String, newValue:Number):Boolean
	class SetGlobalValue : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs != 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_Number) return;

			TESForm* targetForm = MCMUtils::GetFormFromIdentifier(args->args[0].GetString());
			TESGlobal* global = nullptr;
			global = DYNAMIC_CAST(targetForm, TESForm, TESGlobal);

			if (global) {
				global->value = args->args[1].GetNumber();
				args->result->SetBool(true);
			}
		}
	};

	// function GetPropertyValue(formIdentifier:String, propertyName:String):*
	// Returns null if the property doesn't exist.
	class GetPropertyValue : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetNull();

			if (args->numArgs < 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			
			const char* formIdentifier	= args->args[0].GetString();
			const char*	propertyName	= args->args[1].GetString();

			VMValue valueOut;
			bool getOK = MCMUtils::GetPropertyValue(formIdentifier, nullptr, propertyName, &valueOut);
			if (getOK)
				PlatformAdapter::ConvertPapyrusValue(args->result, &valueOut, args->movie->movieRoot);
		}
	};

	// function SetPropertyValue(formIdentifier:String, propertyName:String, newValue:*):Boolean
	class SetPropertyValue : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;

			const char* formIdentifier	= args->args[0].GetString();
			const char*	propertyName	= args->args[1].GetString();
			GFxValue*	newValue		= &args->args[2];

			VirtualMachine* vm = (*G::gameVM)->m_virtualMachine;

			VMValue newVMValue;
			PlatformAdapter::ConvertScaleformValue(&newVMValue, newValue, vm);
			bool setOK = MCMUtils::SetPropertyValue(formIdentifier, nullptr, propertyName, &newVMValue);

			args->result->SetBool(setOK);
		}
	};

	// function GetPropertyValueEx(formIdentifier:String, scriptName:String, propertyName:String):*
	// Returns null if the property doesn't exist.
	class GetPropertyValueEx : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetNull();

			if (args->numArgs < 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			if (args->args[2].GetType() != GFxValue::kType_String) return;
			
			const char* formIdentifier	= args->args[0].GetString();
			const char*	scriptName		= args->args[1].GetString();
			const char*	propertyName	= args->args[2].GetString();

			VMValue valueOut;
			bool getOK = MCMUtils::GetPropertyValue(formIdentifier, scriptName, propertyName, &valueOut);
			if (getOK)
				PlatformAdapter::ConvertPapyrusValue(args->result, &valueOut, args->movie->movieRoot);
		}
	};

	// function SetPropertyValueEx(formIdentifier:String, scriptName:String, propertyName:String, newValue:*):Boolean
	class SetPropertyValueEx : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 4) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			if (args->args[2].GetType() != GFxValue::kType_String) return;
			if (args->args[3].GetType() != GFxValue::kType_String) return;

			const char* formIdentifier	= args->args[0].GetString();
			const char*	scriptName		= args->args[1].GetString();
			const char*	propertyName	= args->args[2].GetString();
			GFxValue*	newValue		= &args->args[3];

			VirtualMachine* vm = (*G::gameVM)->m_virtualMachine;

			VMValue newVMValue;
			PlatformAdapter::ConvertScaleformValue(&newVMValue, newValue, vm);
			bool setOK = MCMUtils::SetPropertyValue(formIdentifier, scriptName, propertyName, &newVMValue);

			args->result->SetBool(setOK);
		}
	};

	// function CallQuestFunction(formID:String, scriptName:String, functionName:String, ...arguments);
	// e.g. CallQuestFunction("MyMod.esp|F99", "MyScript", "MyFunction", 0.1, 0.2, true);
	// Note: this function has been updated to accept any Form type.
	class CallQuestFunction : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return; // formIdentifier
			if (args->args[1].GetType() != GFxValue::kType_String) return; // scriptName
			if (args->args[2].GetType() != GFxValue::kType_String) return; // functionName

			TESForm* targetForm = MCMUtils::GetFormFromIdentifier(args->args[0].GetString());

			if (!targetForm) {
				_MESSAGE("WARNING: %s is not a valid form.", args->args[0].GetString());
			} else {
				const char* scriptName = args->args[1].GetString();

				VirtualMachine* vm = (*G::gameVM)->m_virtualMachine;
				MCMUtils::VMScript script(targetForm, scriptName);

				if (!script.m_identifier) {
					_MESSAGE("WARNING: %s cannot be resolved to a Papyrus script object.", args->args[0].GetString());
				} else {
					BSFixedString funcName(args->args[2].GetString());

					VMValue packedArgs;
					UInt32 length = args->numArgs - 3;
					VMValue::ArrayData* arrayData = nullptr;
					vm->CreateArray(&packedArgs, length, &arrayData);

					packedArgs.type.value = VMValue::kType_VariableArray;
					packedArgs.data.arr = arrayData;

					for (UInt32 i = 0; i < length; i++)
					{
						VMValue* var = new VMValue;
						PlatformAdapter::ConvertScaleformValue(var, &args->args[i + 3], vm);
						arrayData->arr.entries[i].SetVariable(var);
					}

					CallFunctionNoWait_Internal(vm, 0, script.m_identifier, &funcName, &packedArgs);

					args->result->SetBool(true);
				}
			}
		}
	};

	// function CallGlobalFunction(scriptName:String, funcName:String, ...arguments);
	// e.g. CallGlobalFunction("Debug", "MessageBox", "Hello world!");
	class CallGlobalFunction : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;

			VirtualMachine* vm = (*G::gameVM)->m_virtualMachine;

			BSFixedString scriptName(args->args[0].GetString());
			BSFixedString funcName(args->args[1].GetString());
			
			VMValue packedArgs;
			UInt32 length = args->numArgs - 2;
			VMValue::ArrayData* arrayData = nullptr;
			vm->CreateArray(&packedArgs, length, &arrayData);

			packedArgs.type.value = VMValue::kType_VariableArray;
			packedArgs.data.arr = arrayData;

			for (UInt32 i = 0; i < length; i++)
			{
				VMValue * var = new VMValue;
				PlatformAdapter::ConvertScaleformValue(var, &args->args[i + 2], vm);
				arrayData->arr.entries[i].SetVariable(var);
			}

			CallGlobalFunctionNoWait_Internal(vm, 0, 0, &scriptName, &funcName, &packedArgs);

			args->result->SetBool(true);
		}
	};

	// GetModSettingInt(modName:String, settingName:String):int;
	class GetModSettingInt : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetNumber(-1);

			if (args->numArgs != 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;

			args->result->SetInt(SettingStore::GetInstance().GetModSettingInt(args->args[0].GetString(), args->args[1].GetString()));
		}
	};

	// GetModSettingBool(modName:String, settingName:String):Boolean;
	class GetModSettingBool : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs != 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;

			args->result->SetBool(SettingStore::GetInstance().GetModSettingBool(args->args[0].GetString(), args->args[1].GetString()));
		}
	};

	// GetModSettingFloat(modName:String, settingName:String):Number;
	class GetModSettingFloat : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetNumber(-1);

			if (args->numArgs != 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;

			args->result->SetNumber(SettingStore::GetInstance().GetModSettingFloat(args->args[0].GetString(), args->args[1].GetString()));
		}
	};

	// GetModSettingString(modName:String, settingName:String):String;
	class GetModSettingString : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetString("");

			if (args->numArgs != 2) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;

			args->result->SetString(SettingStore::GetInstance().GetModSettingString(args->args[0].GetString(), args->args[1].GetString()));
		}
	};

	// SetModSettingInt(modName:String, settingName:String, value:int):Boolean;
	class SetModSettingInt : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs != 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			if (args->args[2].GetType() != GFxValue::kType_Int) return;

			SettingStore::GetInstance().SetModSettingInt(args->args[0].GetString(), args->args[1].GetString(), args->args[2].GetInt());

			args->result->SetBool(true);
		}
	};

	// SetModSettingBool(modName:String, settingName:String, value:Boolean):Boolean;
	class SetModSettingBool : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs != 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			if (args->args[2].GetType() != GFxValue::kType_Bool) return;

			SettingStore::GetInstance().SetModSettingBool(args->args[0].GetString(), args->args[1].GetString(), args->args[2].GetBool());

			args->result->SetBool(true);
		}
	};

	// SetModSettingFloat(modName:String, settingName:String, value:Number):Boolean;
	class SetModSettingFloat : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs != 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			if (args->args[2].GetType() != GFxValue::kType_Number) return;

			SettingStore::GetInstance().SetModSettingFloat(args->args[0].GetString(), args->args[1].GetString(), args->args[2].GetNumber());

			args->result->SetBool(true);
		}
	};

	// SetModSettingString(modName:String, settingName:String, value:String):Boolean;
	class SetModSettingString : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs != 3) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			if (args->args[1].GetType() != GFxValue::kType_String) return;
			if (args->args[2].GetType() != GFxValue::kType_String) return;

			SettingStore::GetInstance().SetModSettingString(args->args[0].GetString(), args->args[1].GetString(), args->args[2].GetString());

			args->result->SetBool(true);
		}
	};

	// IsPluginInstalled(modName:String):Boolean;
	class IsPluginInstalled : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 1) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;

			const ModInfo* mi = (*G::dataHandler)->LookupModByName(args->args[0].GetString());
			// modIndex == -1 for mods that are present in the Data directory but not active.
			if (mi && mi->modIndex != 0xFF) {
				args->result->SetBool(true);
			}
		}
	};

	class GetKeybind : public GFxFunctionHandler {
	public:
		enum CallType {
			kCallType_ID,
			kCallType_Keycode,
		};

		virtual void Invoke(Args* args) {
			CallType callType = kCallType_ID;

			if (args->numArgs < 2) return;
			if (args->args[0].GetType() == GFxValue::kType_String && args->args[1].GetType() == GFxValue::kType_String) {
				callType = kCallType_ID;
			} else if (args->args[0].GetType() == GFxValue::kType_Int && args->args[1].GetType() == GFxValue::kType_Int) {
				callType = kCallType_Keycode;
			} else {
				// Invalid parameter type
				return;
			}

			GFxValue keycode, modifiers, keybindType, keybindID, keybindName, modName, type, flags, targetForm, callbackName;
			
			g_keybindManager.Lock();
			KeybindInfo ki;
			if (callType == kCallType_ID) {
				ki = g_keybindManager.GetKeybind(args->args[0].GetString(), args->args[1].GetString());
			} else {
				Keybind kb = {};
				kb.keycode = args->args[0].GetInt();
				kb.modifiers = args->args[1].GetInt();
				ki = g_keybindManager.GetKeybind(kb);
			}
			g_keybindManager.Release();

			SetKeybindInfo(ki, args->movie->movieRoot, args->result);
		}
	};

	class GetAllKeybinds : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			g_keybindManager.Lock();
			std::vector<KeybindInfo> keybinds = g_keybindManager.GetAllKeybinds();
			g_keybindManager.Release();

			args->movie->movieRoot->CreateArray(args->result);

			for (int i = 0; i < keybinds.size(); i++) {
				GFxValue keybindInfoValue;
				SetKeybindInfo(keybinds[i], args->movie->movieRoot, &keybindInfoValue);
				args->result->PushBack(&keybindInfoValue);
			}
		}
	};

	// function SetKeybind(modName:String, keybindID:String, keycode:int, modifiers:int):Boolean
	class SetKeybind : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 4) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;	// modName
			if (args->args[1].GetType() != GFxValue::kType_String) return;	// keybindID
			if (args->args[2].GetType() != GFxValue::kType_Int) return;		// keycode
			if (args->args[3].GetType() != GFxValue::kType_Int) return;		// modifiers

			std::string modName		= args->args[0].GetString();
			std::string keybindID	= args->args[1].GetString();

			Keybind kb = {};
			kb.keycode		= args->args[2].GetInt();
			kb.modifiers	= args->args[3].GetInt();

			if (g_keybindManager.RegisterKeybind(kb, modName.c_str(), keybindID.c_str())) {
				args->result->SetBool(true);
			}
		}
	};

	// function SetKeybindEx(modName:String, keybindID:String, keybindName:String, keycode:int, modifiers:int, type:int, params:Array)
	class SetKeybindEx : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			if (args->numArgs < 5) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;	// modName
			if (args->args[1].GetType() != GFxValue::kType_String) return;	// keybindID
			if (args->args[2].GetType() != GFxValue::kType_String) return;	// keybindDesc
			if (args->args[3].GetType() != GFxValue::kType_Int) return;		// keycode
			if (args->args[4].GetType() != GFxValue::kType_Int) return;		// modifiers
			if (args->args[5].GetType() != GFxValue::kType_Int) return;		// type
			if (args->args[6].GetType() != GFxValue::kType_Array) return;	// params

			Keybind				kb = {};
			KeybindParameters	kp = {};
			kp.modName		= args->args[0].GetString();
			kp.keybindID	= args->args[1].GetString();
			kp.keybindDesc	= args->args[2].GetString();
			kb.keycode		= args->args[3].GetInt();
			kb.modifiers	= args->args[4].GetInt();
			kp.type			= args->args[5].GetInt();
			GFxValue params = args->args[6];
			int paramSize	= params.GetArraySize();

			switch (kp.type) {
				case KeybindParameters::kType_CallFunction:
				{
					if (paramSize < 2) return;
					GFxValue targetFormIdentifier, callbackName;
					params.GetElement(0, &targetFormIdentifier);
					params.GetElement(1, &callbackName);

					if (targetFormIdentifier.GetType()	!= GFxValue::kType_String) return;
					if (callbackName.GetType()			!= GFxValue::kType_String) return;

					TESForm* targetForm = MCMUtils::GetFormFromIdentifier(targetFormIdentifier.GetString());
					if (!targetForm) {
						_WARNING("Cannot register a None form as a call target.");
						return;
					}

					kp.targetFormID = targetForm->formID;
					kp.callbackName = callbackName.GetString();

					g_keybindManager.Register(kb, kp);

					_MESSAGE("Succesfully registered kType_CallFunction keybind for keycode %d.", kb.keycode);

					break;
				}
				case KeybindParameters::kType_CallGlobalFunction:
				{
					if (paramSize < 2) return;
					GFxValue scriptName, functionName;
					params.GetElement(0, &scriptName);
					params.GetElement(1, &functionName);

					if (scriptName.GetType() != GFxValue::kType_String) return;
					if (functionName.GetType() != GFxValue::kType_String) return;

					kp.scriptName	= scriptName.GetString();
					kp.callbackName = functionName.GetString();

					g_keybindManager.Register(kb, kp);

					_MESSAGE("Succesfully registered kType_CallGlobalFunction keybind for keycode %d.", kb.keycode);

					break;
				}
				case KeybindParameters::kType_RunConsoleCommand:
				{
					if (paramSize < 1) return;
					GFxValue consoleCommandValue;
					params.GetElement(0, &consoleCommandValue);

					if (consoleCommandValue.GetType() != GFxValue::kType_String) return;

					kp.callbackName = consoleCommandValue.GetString();

					g_keybindManager.Register(kb, kp);

					_MESSAGE("Succesfully registered kType_RunConsoleCommand keybind for keycode %d.", kb.keycode);

					break;
				}
				case KeybindParameters::kType_SendEvent:
				{
					_MESSAGE("Not implemented.");
					break;
				}
				default:
					_WARNING("Failed to register keybind. Unknown keybind type.");
			}
		}
	};

	class ClearKeybind : public GFxFunctionHandler {
	public:
		enum CallType {
			kCallType_ID,
			kCallType_Keycode,
		};

		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			CallType callType = kCallType_ID;
			if (args->numArgs < 2) return;
			if (args->args[0].GetType() == GFxValue::kType_String && args->args[1].GetType() == GFxValue::kType_String) {
				callType = kCallType_ID;
			} else if (args->args[0].GetType() == GFxValue::kType_Int && args->args[1].GetType() == GFxValue::kType_Int) {
				callType = kCallType_Keycode;
			} else {
				// Invalid parameter type
				return;
			}

			g_keybindManager.Lock();
			if (callType == kCallType_ID) {
				if (g_keybindManager.ClearKeybind(args->args[0].GetString(), args->args[1].GetString()))
					args->result->SetBool(true);
			} else {
				Keybind kb = {};
				kb.keycode		= args->args[0].GetInt();
				kb.modifiers	= args->args[1].GetInt();
				if (g_keybindManager.ClearKeybind(kb))
					args->result->SetBool(true);
			}
			g_keybindManager.Release();
		}
	};

	class RemapKeybind : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetBool(false);

			if (args->numArgs < 4) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return;	// modName
			if (args->args[1].GetType() != GFxValue::kType_String) return;	// keybindID
			if (args->args[2].GetType() != GFxValue::kType_Int) return;		// newKeycode
			if (args->args[3].GetType() != GFxValue::kType_Int) return;		// newModifiers

			Keybind kb		= {};
			kb.keycode		= args->args[2].GetInt();
			kb.modifiers	= args->args[3].GetInt();

			g_keybindManager.Lock();
			if (g_keybindManager.RemapKeybind(args->args[0].GetString(), args->args[1].GetString(), kb))
				args->result->SetBool(true);
			g_keybindManager.Release();
		}
	};

	class GetFullName : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetString("");
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			TESForm* form = MCMUtils::GetFormFromIdentifier(args->args[0].GetString());
			if (!form) return;
			TESFullName* fullname = DYNAMIC_CAST(form, TESForm, TESFullName);
			if (!fullname) return;
			args->result->SetString(fullname->name.c_str());
		}
	};

	class GetDescription : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->result->SetString("");
			if (args->args[0].GetType() != GFxValue::kType_String) return;
			TESForm* form = MCMUtils::GetFormFromIdentifier(args->args[0].GetString());
			if (!form) return;
			args->result->SetString(MCMUtils::GetDescription(form).c_str());
		}
	};

	// function GetListFromForm(formIdentifier:String):Array<String>
	class GetListFromForm : public GFxFunctionHandler {
	public:
		virtual void Invoke(Args* args) {
			args->movie->movieRoot->CreateArray(args->result);

			if (args->numArgs < 1) return;
			if (args->args[0].GetType() != GFxValue::kType_String) return; // formIdentifier

			TESForm* form = MCMUtils::GetFormFromIdentifier(args->args[0].GetString());
			if (!form) return;
			BGSListForm* formlist = DYNAMIC_CAST(form, TESForm, BGSListForm);
			if (!formlist) return;
			
			for (size_t i = 0; i < formlist->forms.count; i++)
			{
				form = formlist->forms.entries[i];
				GFxValue value;
				args->movie->movieRoot->CreateString(&value, "");
				TESFullName* fullname = DYNAMIC_CAST(form, TESForm, TESFullName);
				value.SetString(fullname ? fullname->name.c_str() : form->GetEditorID());
				args->result->PushBack(&value);
			}
		}
	};
}

void ScaleformMCM::RegisterFuncs(GFxValue* codeObj, GFxMovieRoot* movieRoot) {
	// MCM Data
	RegisterFunction<GetMCMVersionString>(codeObj, movieRoot, "GetMCMVersionString");
	RegisterFunction<GetMCMVersionCode>(codeObj, movieRoot, "GetMCMVersionCode");
	RegisterFunction<GetConfigList>(codeObj, movieRoot, "GetConfigList");

	// MCM Events
	RegisterFunction<OnMCMOpen>(codeObj, movieRoot, "OnMCMOpen");
	RegisterFunction<OnMCMClose>(codeObj, movieRoot, "OnMCMClose");

	// MCM Utilities
	RegisterFunction<DisableMenuInput>(codeObj, movieRoot, "DisableMenuInput");

	// Actions
	RegisterFunction<GetGlobalValue>(codeObj, movieRoot, "GetGlobalValue");
	RegisterFunction<SetGlobalValue>(codeObj, movieRoot, "SetGlobalValue");
	RegisterFunction<GetPropertyValue>(codeObj, movieRoot, "GetPropertyValue");
	RegisterFunction<SetPropertyValue>(codeObj, movieRoot, "SetPropertyValue");
	RegisterFunction<GetPropertyValueEx>(codeObj, movieRoot, "GetPropertyValueEx");
	RegisterFunction<SetPropertyValueEx>(codeObj, movieRoot, "SetPropertyValueEx");
	RegisterFunction<CallQuestFunction>(codeObj, movieRoot, "CallQuestFunction");
	RegisterFunction<CallGlobalFunction>(codeObj, movieRoot, "CallGlobalFunction");

	// Mod Settings
	RegisterFunction<GetModSettingInt>(codeObj, movieRoot, "GetModSettingInt");
	RegisterFunction<GetModSettingBool>(codeObj, movieRoot, "GetModSettingBool");
	RegisterFunction<GetModSettingFloat>(codeObj, movieRoot, "GetModSettingFloat");
	RegisterFunction<GetModSettingString>(codeObj, movieRoot, "GetModSettingString");

	RegisterFunction<SetModSettingInt>(codeObj, movieRoot, "SetModSettingInt");
	RegisterFunction<SetModSettingBool>(codeObj, movieRoot, "SetModSettingBool");
	RegisterFunction<SetModSettingFloat>(codeObj, movieRoot, "SetModSettingFloat");
	RegisterFunction<SetModSettingString>(codeObj, movieRoot, "SetModSettingString");

	// Mod Info
	RegisterFunction<IsPluginInstalled>(codeObj, movieRoot, "IsPluginInstalled");

	// Keybinds
	RegisterFunction<GetKeybind>(codeObj, movieRoot, "GetKeybind");
	RegisterFunction<GetAllKeybinds>(codeObj, movieRoot, "GetAllKeybinds");
	RegisterFunction<SetKeybind>(codeObj, movieRoot, "SetKeybind");
	RegisterFunction<ClearKeybind>(codeObj, movieRoot, "ClearKeybind");
	RegisterFunction<RemapKeybind>(codeObj, movieRoot, "RemapKeybind");

	// 
	RegisterFunction<GetFullName>(codeObj, movieRoot, "GetFullName");
	RegisterFunction<GetDescription>(codeObj, movieRoot, "GetDescription");
	RegisterFunction<GetListFromForm>(codeObj, movieRoot, "GetListFromForm");
}

//-------------------------
// Input Handler
//-------------------------
class F4SEInputHandler : public BSInputEventUser
{
public:
	F4SEInputHandler() : BSInputEventUser(true), m_lastThumbstickDirectionLeft(0), m_lastThumbstickDirectionRight(0) { }

	virtual void OnButtonEvent(ButtonEvent * inputEvent)
	{
		// Poll VR controllers directly - bypasses game's broken VR→gamepad translation
		MCMVRInput::Update();

		UInt32	keyCode;
		UInt32	deviceType = inputEvent->deviceType;
		UInt32	keyMask = inputEvent->keyMask;

		float timer	 = inputEvent->timer;
		bool  isDown = inputEvent->isDown == 1.0f && timer == 0.0f;
		bool  isUp   = inputEvent->isDown == 0.0f && timer != 0.0f;

		BSFixedString* control = inputEvent->GetControlID();
		const char* controlName = control ? control->c_str() : "";

		// VR Controller handling (deviceType 4 = Kinect/VR)
		if (deviceType == 4) {
			// Debug: Log VR button events
			static int vrLogCount = 0;
			if (vrLogCount < 50) {
				_MESSAGE("MCM VR Button: control='%s' keyMask=%u isDown=%.1f timer=%.2f", 
					controlName, keyMask, inputEvent->isDown, timer);
				vrLogCount++;
			}

			// Translate VR control names to MCM actions
			const char* mcmControl = nullptr;
			UInt32 mcmKeyCode = 0;

			if (strcmp(controlName, "WandTrigger") == 0 || 
			    strcmp(controlName, "SecondaryTrigger") == 0) {
				// Trigger = Accept/Select
				mcmControl = "Accept";
				mcmKeyCode = InputMap::kGamepadButtonOffset_A;
			}
			else if (strcmp(controlName, "WandGrip") == 0 ||
			         strcmp(controlName, "SecondaryGrip") == 0 ||
			         strcmp(controlName, "Grip") == 0 ||
			         keyMask == 34) {  // keyMask 34 = grip on VR
				// Grip = Tab Left (go back one menu level, NOT close entire menu)
				mcmControl = "LShoulder";
				mcmKeyCode = InputMap::kGamepadButtonOffset_LEFT_SHOULDER;
			}
			// Thumbstick directions (these may come as ButtonEvents in VR)
			else if (strcmp(controlName, "Forward") == 0) {
				mcmControl = "Up";
				mcmKeyCode = InputMap::kGamepadButtonOffset_DPAD_UP;
			}
			else if (strcmp(controlName, "Back") == 0) {
				mcmControl = "Down";
				mcmKeyCode = InputMap::kGamepadButtonOffset_DPAD_DOWN;
			}
			else if (strcmp(controlName, "StrafeLeft") == 0) {
				mcmControl = "Left";
				mcmKeyCode = InputMap::kGamepadButtonOffset_DPAD_LEFT;
			}
			else if (strcmp(controlName, "StrafeRight") == 0) {
				mcmControl = "Right";
				mcmKeyCode = InputMap::kGamepadButtonOffset_DPAD_RIGHT;
			}

			if (mcmControl) {
				if (isDown) {
					ScaleformMCM::ProcessKeyEvent(mcmKeyCode, true);
					ScaleformMCM::ProcessUserEvent(mcmControl, true, InputEvent::kDeviceType_Gamepad);
				} else if (isUp) {
					ScaleformMCM::ProcessKeyEvent(mcmKeyCode, false);
					ScaleformMCM::ProcessUserEvent(mcmControl, false, InputEvent::kDeviceType_Gamepad);
					
					// After grip release (go back), clear submenu selection and fix highlight
					if (strcmp(mcmControl, "LShoulder") == 0) {
						_MESSAGE("MCM Grip released - calling GoBackOneMenu");
						ScaleformMCM::GoBackOneMenu();
					}
				}
			}
			return;  // Don't process VR events through normal path
		}

		if (deviceType == InputEvent::kDeviceType_Mouse) {
			// Mouse
			if (keyMask < 2 || keyMask > 7) return;	// Disallow Mouse1, Mouse2, MouseWheelUp and MouseWheelDown
			keyCode = InputMap::kMacro_MouseButtonOffset + keyMask;
		} else if (deviceType == InputEvent::kDeviceType_Gamepad) {
			// Gamepad
			keyCode = InputMap::GamepadMaskToKeycode(keyMask);
		} else {
			// Keyboard
			keyCode = keyMask;
		}

		if (isDown) {
			ScaleformMCM::ProcessKeyEvent(keyCode, true);
			ScaleformMCM::ProcessUserEvent(controlName, true, deviceType);
		} else if (isUp) {
			ScaleformMCM::ProcessKeyEvent(keyCode, false);
			ScaleformMCM::ProcessUserEvent(controlName, false, deviceType);
		}
	}

	// VR Fix: Handle thumbstick events for menu navigation
	virtual void OnThumbstickEvent(ThumbstickEvent * inputEvent)
	{
		// NOTE: We do NOT call MCMVRInput::Update() here anymore.
		// We now directly call NavigateList() which is more reliable.
		// Calling both would cause double-navigation on some controllers.

		if (!inputEvent) return;

		// Process BOTH thumbsticks for menu navigation
		// In pause menu, right stick has no other function, so use it for navigation too
		
		// Debug: Log thumbstick events only when direction changes
		if (inputEvent->direction != inputEvent->previousDirection) {
			_MESSAGE("MCM Thumbstick: stick=0x%X prevDir=%u dir=%u x=%.2f y=%.2f", 
				inputEvent->stick, inputEvent->previousDirection, inputEvent->direction,
				inputEvent->x, inputEvent->y);
		}

		// Track both sticks independently to avoid conflicts
		UInt32& lastDirection = (inputEvent->stick == 0xB) ? 
			m_lastThumbstickDirectionLeft : m_lastThumbstickDirectionRight;

		// Thumbstick directions: 0=none, 1=up, 2=right, 3=down, 4=left
		UInt32 newDirection = inputEvent->direction;
		UInt32 previousDirection = inputEvent->previousDirection;

		// Only process when direction changes
		if (newDirection == previousDirection) return;

		// Release previous direction
		if (lastDirection != 0) {
			const char* releaseControl = GetControlNameForDirection(lastDirection);
			if (releaseControl) {
				UInt32 keyCode = GetKeycodeForDirection(lastDirection);
				ScaleformMCM::ProcessKeyEvent(keyCode, false);
				ScaleformMCM::ProcessUserEvent(releaseControl, false, InputEvent::kDeviceType_Gamepad);
			}
		}

		// Press new direction - call list navigation directly
		if (newDirection != 0) {
			const char* pressControl = GetControlNameForDirection(newDirection);
			if (pressControl) {
				_MESSAGE("MCM Thumbstick PRESS: stick=0x%X dir=%u control='%s'", inputEvent->stick, newDirection, pressControl);
				
				// Left thumbstick (0xB) only handles left/right for slider control
				// Up/down navigation is handled by right thumbstick only
				bool isLeftStick = (inputEvent->stick == 0xB);
				bool isUpDown = (newDirection == 1 || newDirection == 3);
				
				if (isLeftStick && isUpDown) {
					// Skip up/down on left thumbstick - don't navigate
					_MESSAGE("MCM Thumbstick: Skipping up/down on left stick");
				} else {
					// Directly call NavigateList for all directions
					// 1=up, 2=right, 3=down, 4=left
					ScaleformMCM::NavigateList(newDirection);
				}
			}
		}

		lastDirection = newDirection;
	}

private:
	UInt32 m_lastThumbstickDirectionLeft;
	UInt32 m_lastThumbstickDirectionRight;

	const char* GetControlNameForDirection(UInt32 direction)
	{
		switch (direction) {
			case 1: return "Up";
			case 2: return "Right";
			case 3: return "Down";
			case 4: return "Left";
			default: return nullptr;
		}
	}

	UInt32 GetKeycodeForDirection(UInt32 direction)
	{
		// MCM's ProcessUserEvent for gamepad (deviceType 2) sends WASD keys, NOT arrow keys!
		// From MCM_Menu.as ProcessUserEvent:
		//   case "Up": this.ProcessKeyEvent(Keyboard.W, false);  // W = 87
		//   case "Down": this.ProcessKeyEvent(Keyboard.S, false);  // S = 83
		//   case "Left": this.ProcessKeyEvent(Keyboard.A, false);  // A = 65
		//   case "Right": this.ProcessKeyEvent(Keyboard.D, false);  // D = 68
		switch (direction) {
			case 1: return 87;  // W key (up)
			case 2: return 68;  // D key (right)
			case 3: return 83;  // S key (down)
			case 4: return 65;  // A key (left)
			default: return 0;
		}
	}

	// Left thumbstick sends gamepad DPAD keycodes - maybe MCM checks for these?
	UInt32 GetLeftThumbstickKeycodeForDirection(UInt32 direction)
	{
		switch (direction) {
			case 1: return 266;  // Gamepad DPAD Up
			case 2: return 267;  // Gamepad DPAD Right  
			case 3: return 268;  // Gamepad DPAD Down
			case 4: return 269;  // Gamepad DPAD Left
			default: return 0;
		}
	}
};
F4SEInputHandler g_scaleformInputHandler;

void ScaleformMCM::ProcessKeyEvent(UInt32 keyCode, bool isDown)
{
	BSFixedString mainMenuStr("PauseMenu");
	if ((*G::ui)->IsMenuOpen(mainMenuStr)) {
		IMenu* menu = (*G::ui)->GetMenu(mainMenuStr);
		GFxMovieRoot* movieRoot = menu->movie->movieRoot;
		GFxValue args[2];
		args[0].SetInt(keyCode);
		args[1].SetBool(isDown);
		_MESSAGE("MCM ProcessKeyEvent: keyCode=%u isDown=%d", keyCode, isDown);
		
		// Try MCM's content handler
		movieRoot->Invoke("root.mcm_loader.content.ProcessKeyEvent", nullptr, args, 2);
		
		// Also try native pause menu paths that might handle navigation
		// These are common Scaleform/Flash patterns for menu navigation
		movieRoot->Invoke("root.Menu_mc.ProcessKeyEvent", nullptr, args, 2);
		movieRoot->Invoke("root.ProcessKeyEvent", nullptr, args, 2);
	} else {
		_MESSAGE("MCM ProcessKeyEvent SKIPPED: PauseMenu not open, keyCode=%u", keyCode);
	}
}

void ScaleformMCM::ProcessUserEvent(const char * controlName, bool isDown, int deviceType)
{
	BSFixedString mainMenuStr("PauseMenu");
	if ((*G::ui)->IsMenuOpen(mainMenuStr)) {
		IMenu* menu = (*G::ui)->GetMenu(mainMenuStr);
		GFxMovieRoot* movieRoot = menu->movie->movieRoot;
		GFxValue args[3];
		args[0].SetString(controlName);
		args[1].SetBool(isDown);
		args[2].SetInt(deviceType);
		movieRoot->Invoke("root.mcm_loader.content.ProcessUserEvent", nullptr, args, 3);
	}
}

// Directly navigate MCM list - bypasses event system for more reliable VR input
// Directions: 1=up, 2=right, 3=down, 4=left
void ScaleformMCM::NavigateList(int direction)
{
	BSFixedString mainMenuStr("PauseMenu");
	if (!(*G::ui)->IsMenuOpen(mainMenuStr)) return;
	
	IMenu* menu = (*G::ui)->GetMenu(mainMenuStr);
	GFxMovieRoot* movieRoot = menu->movie->movieRoot;
	
	// Get the MCM menu content
	GFxValue mcmContent;
	if (!movieRoot->GetVariable(&mcmContent, "root.mcm_loader.content.mcmMenu")) {
		_MESSAGE("MCM NavigateList: Failed to get mcmMenu");
		return;
	}
	
	// Get references to both lists
	GFxValue configPanel, configList, helpPanel, helpList;
	bool hasConfigList = mcmContent.GetMember("configPanel_mc", &configPanel) && 
	                     configPanel.GetMember("configList_mc", &configList);
	bool hasHelpList = mcmContent.GetMember("HelpPanel_mc", &helpPanel) && 
	                   helpPanel.GetMember("HelpList_mc", &helpList);
	
	// Check selectedIndex on both lists
	// MCM sets selectedIndex = -1 on the inactive list when switching focus
	int configIndex = -1, helpIndex = -1;
	
	if (hasConfigList) {
		GFxValue idx;
		if (configList.GetMember("selectedIndex", &idx) && 
		    (idx.GetType() == GFxValue::kType_Int || idx.GetType() == GFxValue::kType_Number)) {
			configIndex = (int)idx.GetNumber();
		}
	}
	
	if (hasHelpList) {
		GFxValue idx;
		if (helpList.GetMember("selectedIndex", &idx) && 
		    (idx.GetType() == GFxValue::kType_Int || idx.GetType() == GFxValue::kType_Number)) {
			helpIndex = (int)idx.GetNumber();
		}
	}
	
	_MESSAGE("MCM NavigateList: dir=%d configIndex=%d, helpIndex=%d", direction, configIndex, helpIndex);
	
	// Determine which list is active
	bool configActive = (hasConfigList && configIndex >= 0);
	bool helpActive = (!configActive && hasHelpList);
	
	// Handle left/right based on which list is active
	if (direction == 4) {  // LEFT
		if (configActive) {
			// In configList: adjust sliders/steppers by calling Decrement() on the OptionItem
			// Path: configList.selectedEntry.clipIndex -> GetClipByIndex -> child OptionItem -> Decrement()
			GFxValue selectedEntry;
			if (configList.GetMember("selectedEntry", &selectedEntry) && !selectedEntry.IsNull() && !selectedEntry.IsUndefined()) {
				_MESSAGE("MCM NavigateList LEFT: Got selectedEntry, type=%d", selectedEntry.GetType());
				GFxValue clipIndex;
				bool hasClipIndex = selectedEntry.GetMember("clipIndex", &clipIndex);
				_MESSAGE("MCM NavigateList LEFT: hasClipIndex=%d, clipIndex type=%d", hasClipIndex, clipIndex.GetType());
				
				// clipIndex can be Int, UInt, Number, or String - handle all cases
				int clipIndexValue = -1;
				if (hasClipIndex) {
					if (clipIndex.GetType() == GFxValue::kType_Int || 
					    clipIndex.GetType() == GFxValue::kType_UInt || 
					    clipIndex.GetType() == GFxValue::kType_Number) {
						clipIndexValue = (int)clipIndex.GetNumber();
						_MESSAGE("MCM NavigateList LEFT: clipIndex from number = %d", clipIndexValue);
					} else if (clipIndex.GetType() == GFxValue::kType_String) {
						const char* str = clipIndex.GetString();
						_MESSAGE("MCM NavigateList LEFT: clipIndex string = '%s'", str ? str : "NULL");
						if (str) clipIndexValue = atoi(str);
					} else {
						_MESSAGE("MCM NavigateList LEFT: clipIndex unknown type %d", clipIndex.GetType());
					}
				}
				_MESSAGE("MCM NavigateList LEFT: clipIndexValue = %d", clipIndexValue);
				
				if (clipIndexValue >= 0) {
					
					_MESSAGE("MCM NavigateList LEFT: clipIndex = %d", clipIndexValue);
					
					GFxValue args[1], settingsOptionItem;
					args[0].SetNumber(clipIndexValue);
					bool gotClip = configList.Invoke("GetClipByIndex", &settingsOptionItem, args, 1);
					_MESSAGE("MCM NavigateList LEFT: GetClipByIndex returned %d, result type=%d", gotClip, settingsOptionItem.GetType());
					
					if (!settingsOptionItem.IsNull() && !settingsOptionItem.IsUndefined()) {
						// SettingsOptionItem has OptionItem as child (added via addChild)
						GFxValue numChildren;
						if (settingsOptionItem.GetMember("numChildren", &numChildren)) {
							int childCount = (int)numChildren.GetNumber();
							_MESSAGE("MCM NavigateList LEFT: SettingsOptionItem has %d children", childCount);
							
							// Try each child - OptionItem is usually added as a child
							bool found = false;
							for (int i = childCount - 1; i >= 0 && !found; i--) {
								GFxValue getChildArgs[1], child;
								getChildArgs[0].SetNumber(i);
								settingsOptionItem.Invoke("getChildAt", &child, getChildArgs, 1);
								
								if (!child.IsNull() && !child.IsUndefined()) {
									// Try to call Decrement - if it works, this is a slider
									GFxValue result;
									bool success = child.Invoke("Decrement", &result, nullptr, 0);
									if (success) {
										_MESSAGE("MCM NavigateList LEFT: Called Decrement on child %d - SUCCESS", i);
										found = true;
										break;
									}
									
									// If not, try to access and decrement "index" for steppers
									GFxValue idx;
									if (child.GetMember("index", &idx) && 
									    (idx.GetType() == GFxValue::kType_Int || idx.GetType() == GFxValue::kType_Number)) {
										int currentIdx = (int)idx.GetNumber();
										if (currentIdx > 0) {
											GFxValue newIdx;
											newIdx.SetNumber(currentIdx - 1);
											child.SetMember("index", &newIdx);
											_MESSAGE("MCM NavigateList LEFT: Decremented stepper index from %d to %d", currentIdx, currentIdx - 1);
											found = true;
											break;
										}
									}
								}
							}
							if (!found) {
								_MESSAGE("MCM NavigateList LEFT: No slider/stepper child found");
							}
						}
					}
				}
			} else {
				_MESSAGE("MCM NavigateList LEFT: No selectedEntry");
			}
			_MESSAGE("MCM NavigateList: LEFT in configList for slider/stepper");
		} else if (helpActive) {
			// In HelpList: go back (same as grip/LShoulder)
			GoBackOneMenu();
			_MESSAGE("MCM NavigateList: LEFT in HelpList = GoBack");
		}
		return;
	}
	
	if (direction == 2) {  // RIGHT
		if (configActive) {
			// In configList: adjust sliders/steppers by calling Increment() on the OptionItem
			GFxValue selectedEntry;
			if (configList.GetMember("selectedEntry", &selectedEntry) && !selectedEntry.IsNull() && !selectedEntry.IsUndefined()) {
				_MESSAGE("MCM NavigateList RIGHT: Got selectedEntry, type=%d", selectedEntry.GetType());
				GFxValue clipIndex;
				bool hasClipIndex = selectedEntry.GetMember("clipIndex", &clipIndex);
				_MESSAGE("MCM NavigateList RIGHT: hasClipIndex=%d, clipIndex type=%d", hasClipIndex, clipIndex.GetType());
				
				// clipIndex can be Int, UInt, Number, or String - handle all cases
				int clipIndexValue = -1;
				if (hasClipIndex) {
					if (clipIndex.GetType() == GFxValue::kType_Int || 
					    clipIndex.GetType() == GFxValue::kType_UInt || 
					    clipIndex.GetType() == GFxValue::kType_Number) {
						clipIndexValue = (int)clipIndex.GetNumber();
						_MESSAGE("MCM NavigateList RIGHT: clipIndex from number = %d", clipIndexValue);
					} else if (clipIndex.GetType() == GFxValue::kType_String) {
						const char* str = clipIndex.GetString();
						_MESSAGE("MCM NavigateList RIGHT: clipIndex string = '%s'", str ? str : "NULL");
						if (str) clipIndexValue = atoi(str);
					} else {
						_MESSAGE("MCM NavigateList RIGHT: clipIndex unknown type %d", clipIndex.GetType());
					}
				}
				_MESSAGE("MCM NavigateList RIGHT: clipIndexValue = %d", clipIndexValue);
				
				if (clipIndexValue >= 0) {
					
					_MESSAGE("MCM NavigateList RIGHT: clipIndex = %d", clipIndexValue);
					
					GFxValue args[1], settingsOptionItem;
					args[0].SetNumber(clipIndexValue);
					bool gotClip = configList.Invoke("GetClipByIndex", &settingsOptionItem, args, 1);
					_MESSAGE("MCM NavigateList RIGHT: GetClipByIndex returned %d, result type=%d", gotClip, settingsOptionItem.GetType());
					
					if (!settingsOptionItem.IsNull() && !settingsOptionItem.IsUndefined()) {
						GFxValue numChildren;
						if (settingsOptionItem.GetMember("numChildren", &numChildren)) {
							int childCount = (int)numChildren.GetNumber();
							_MESSAGE("MCM NavigateList RIGHT: SettingsOptionItem has %d children", childCount);
							
							bool found = false;
							for (int i = childCount - 1; i >= 0 && !found; i--) {
								GFxValue getChildArgs[1], child;
								getChildArgs[0].SetNumber(i);
								settingsOptionItem.Invoke("getChildAt", &child, getChildArgs, 1);
								
								if (!child.IsNull() && !child.IsUndefined()) {
									// Try to call Increment - if it works, this is a slider
									GFxValue result;
									bool success = child.Invoke("Increment", &result, nullptr, 0);
									if (success) {
										_MESSAGE("MCM NavigateList RIGHT: Called Increment on child %d - SUCCESS", i);
										found = true;
										break;
									}
									
									// If not, try to access and increment "index" for steppers
									GFxValue idx;
									if (child.GetMember("index", &idx) && 
									    (idx.GetType() == GFxValue::kType_Int || idx.GetType() == GFxValue::kType_Number)) {
										int currentIdx = (int)idx.GetNumber();
										// TODO: check max index
										GFxValue newIdx;
										newIdx.SetNumber(currentIdx + 1);
										child.SetMember("index", &newIdx);
										_MESSAGE("MCM NavigateList RIGHT: Incremented stepper index from %d to %d", currentIdx, currentIdx + 1);
										found = true;
										break;
									}
								}
							}
							if (!found) {
								_MESSAGE("MCM NavigateList RIGHT: No slider/stepper child found");
							}
						}
					}
				}
			} else {
				_MESSAGE("MCM NavigateList RIGHT: No selectedEntry");
			}
			_MESSAGE("MCM NavigateList: RIGHT in configList for slider/stepper");
		} else if (helpActive) {
			// In HelpList: enter submenu (same as trigger/RShoulder)
			GFxValue result;
			mcmContent.Invoke("RShoulderPressed", &result, nullptr, 0);
			_MESSAGE("MCM NavigateList: RIGHT in HelpList = Enter submenu");
		}
		return;
	}
	
	// Handle up/down - navigate the active list
	GFxValue* targetList = nullptr;
	const char* listName = nullptr;
	
	if (configActive) {
		targetList = &configList;
		listName = "configList_mc";
	} else if (helpActive) {
		targetList = &helpList;
		listName = "HelpList_mc";
	}
	
	if (targetList) {
		const char* method = (direction == 1) ? "moveSelectionUp" : "moveSelectionDown";
		GFxValue result;
		targetList->Invoke(method, &result, nullptr, 0);
		_MESSAGE("MCM NavigateList: Called %s.%s", listName, method);
	} else {
		_MESSAGE("MCM NavigateList: Could not determine which list to navigate");
	}
}

void ScaleformMCM::RefreshMenu()
{
	BSFixedString mainMenuStr("PauseMenu");
	if ((*G::ui)->IsMenuOpen(mainMenuStr)) {
		IMenu* menu = (*G::ui)->GetMenu(mainMenuStr);
		GFxMovieRoot* movieRoot = menu->movie->movieRoot;
		movieRoot->Invoke("root.mcm_loader.content.RefreshMCM", nullptr, nullptr, 0);
	}
}

// Called after grip/LShoulder to properly clear submenu selection and update highlight
// On the root menu, this will close the menu instead of going back
void ScaleformMCM::GoBackOneMenu()
{
	// Debounce - ignore if called within 200ms of last call
	auto currentTime = std::chrono::steady_clock::now();
	if (g_goBackTimeInitialized) {
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - g_lastGoBackTime).count();
		if (elapsed < 200) {
			_MESSAGE("MCM GoBackOneMenu: Debounced (within 200ms of last call, elapsed=%lld)", elapsed);
			return;
		}
	}
	g_lastGoBackTime = currentTime;
	g_goBackTimeInitialized = true;
	
	BSFixedString mainMenuStr("PauseMenu");
	if (!(*G::ui)->IsMenuOpen(mainMenuStr)) return;
	
	IMenu* menu = (*G::ui)->GetMenu(mainMenuStr);
	GFxMovieRoot* movieRoot = menu->movie->movieRoot;
	
	// Check if we're on the root menu by looking at configPanel's selectedIndex
	// On root menu: configList has selectedIndex = -1 (no mod selected)
	// In a mod's settings: configList has selectedIndex >= 0
	GFxValue configList;
	bool isOnRootMenu = true;  // Assume root menu
	
	if (movieRoot->GetVariable(&configList, "root.mcm_loader.content.mcmMenu.configPanel_mc.configList_mc")) {
		GFxValue selectedIndex;
		if (configList.GetMember("selectedIndex", &selectedIndex)) {
			int idx = -1;
			if (selectedIndex.GetType() == GFxValue::kType_Int || 
			    selectedIndex.GetType() == GFxValue::kType_UInt || 
			    selectedIndex.GetType() == GFxValue::kType_Number) {
				idx = (int)selectedIndex.GetNumber();
			}
			_MESSAGE("MCM GoBackOneMenu: configList selectedIndex = %d (type=%d)", idx, selectedIndex.GetType());
			if (idx >= 0) {
				isOnRootMenu = false;  // Has selection = inside a mod's settings
			}
		} else {
			_MESSAGE("MCM GoBackOneMenu: Could not get selectedIndex");
		}
	} else {
		_MESSAGE("MCM GoBackOneMenu: Could not get configList");
	}
	
	if (isOnRootMenu) {
		// On root menu - send Cancel event to close the menu
		_MESSAGE("MCM GoBackOneMenu: On root menu - sending Cancel to close");
		GFxValue args[3];
		args[0].SetString("Cancel");
		args[1].SetBool(true);  // isDown
		args[2].SetInt(InputEvent::kDeviceType_Gamepad);
		movieRoot->Invoke("root.mcm_loader.content.ProcessUserEvent", nullptr, args, 3);
		
		// Also send release
		args[1].SetBool(false);  // isUp
		movieRoot->Invoke("root.mcm_loader.content.ProcessUserEvent", nullptr, args, 3);
		return;
	}
	
	// Not on root menu - go back one level as before
	// Simply call LShoulderPressed on MCM menu - it handles everything properly
	// except for clearing configList.selectedIndex, which we handle separately
	GFxValue result;
	movieRoot->Invoke("root.mcm_loader.content.mcmMenu.LShoulderPressed", &result, nullptr, 0);
	_MESSAGE("MCM GoBackOneMenu: Called LShoulderPressed()");
	
	// Clear configList_mc.selectedIndex to -1 since LShoulderPressed doesn't do this
	{
		GFxValue minusOne;
		minusOne.SetInt(-1);
		configList.SetMember("selectedIndex", &minusOne);
		
		// Call InvalidateData to refresh visuals (clears highlight)
		configList.Invoke("InvalidateData", &result, nullptr, 0);
		_MESSAGE("MCM GoBackOneMenu: Cleared configList selectedIndex and invalidated");
	}
	
	// Refresh HelpList to show its highlight (focus is now on HelpList)
	GFxValue helpList;
	if (movieRoot->GetVariable(&helpList, "root.mcm_loader.content.mcmMenu.HelpPanel_mc.HelpList_mc")) {
		// InvalidateData forces a full refresh of all entries including highlight state
		helpList.Invoke("InvalidateData", &result, nullptr, 0);
		_MESSAGE("MCM GoBackOneMenu: Refreshed HelpList");
	}
}

void ScaleformMCM::SetKeybindInfo(KeybindInfo ki, GFxMovieRoot * movieRoot, GFxValue * kiValue)
{
	GFxValue keycode, modifiers, keybindType, keybindID, keybindName, modName, type, flags, targetForm, callbackName;

	keycode.SetInt(ki.keycode);
	modifiers.SetInt(ki.modifiers);
	keybindType.SetInt(ki.keybindType);
	keybindID.SetString(ki.keybindID);
	keybindName.SetString(ki.keybindDesc);
	modName.SetString(ki.modName);
	type.SetInt(ki.type);
	flags.SetInt(ki.flags);
	targetForm.SetString(ki.callTarget);
	callbackName.SetString(ki.callbackName);

	movieRoot->CreateObject(kiValue);
	kiValue->SetMember("keycode", &keycode);
	kiValue->SetMember("modifiers", &modifiers);
	kiValue->SetMember("keybindType", &keybindType);
	kiValue->SetMember("keybindID", &keybindID);
	kiValue->SetMember("keybindName", &keybindName);
	kiValue->SetMember("modName", &modName);
	kiValue->SetMember("type", &type);
	kiValue->SetMember("flags", &flags);
	kiValue->SetMember("targetForm", &targetForm);
	kiValue->SetMember("callbackName", &callbackName);
}

void ScaleformMCM::RegisterForInput(bool bRegister) {
	if (bRegister) {
		g_scaleformInputHandler.enabled = true;
		tArray<BSInputEventUser*>* inputEvents = &((*G::menuControls)->inputEvents);
		BSInputEventUser* inputHandler = &g_scaleformInputHandler;
		int idx = inputEvents->GetItemIndex(inputHandler);
		if (idx == -1) {
			inputEvents->Push(&g_scaleformInputHandler);
			_MESSAGE("Registered for input events.");
		}
	} else {
		g_scaleformInputHandler.enabled = false;
	}
}

bool ScaleformMCM::RegisterScaleform(GFxMovieView * view, GFxValue * f4se_root)
{
	GFxMovieRoot* movieRoot = view->movieRoot;

	GFxValue currentSWFPath;
	const char* currentSWFPathString = nullptr;

	if (movieRoot->GetVariable(&currentSWFPath, "root.loaderInfo.url")) {
		currentSWFPathString = currentSWFPath.GetString();
	} else {
		_MESSAGE("WARNING: Scaleform registration failed.");
	}
	// Look for the menu that we want to inject into.
	if (strcmp(currentSWFPathString, "Interface/world_MainMenu.swf") == 0) {
        GFxValue root; movieRoot->GetVariable(&root, "root");

        // Register native code object
        GFxValue mcm; movieRoot->CreateObject(&mcm);
        root.SetMember("mcm", &mcm);
        ScaleformMCM::RegisterFuncs(&mcm, movieRoot);

		// Inject MCM menu
		GFxValue loader, urlRequest;
		movieRoot->CreateObject(&loader, "flash.display.Loader");
		movieRoot->CreateObject(&urlRequest, "flash.net.URLRequest", &GFxValue("MCM_VR.swf"), 1);

		root.SetMember("mcm_loader", &loader);
		bool injectionSuccess = movieRoot->Invoke("root.mcm_loader.load", nullptr, &urlRequest, 1);

		movieRoot->Invoke("root.Menu_mc.addChild", nullptr, &loader, 1);

		if (!injectionSuccess) {
			_MESSAGE("WARNING: MCM injection failed.");
		}
	}

	return true;
}