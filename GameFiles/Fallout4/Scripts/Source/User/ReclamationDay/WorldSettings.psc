Scriptname ReclamationDay:WorldSettings Hidden
{Reclamation Day's WORLD SETTINGS (the F2 overlay's; the page is Data\ReclamationDay\WorldSettings.json, run by the
client's WorldSettings.cpp): the host's weather, and NAC X's settings, which NAC only offers on its holotape. The page
sets NAC's globals itself; these are the holotape's buttons that do more than set one (a refresh after, a camera switch,
a weather loaded or banned), as the holotape does them (NAC:MFXConfigSettings) without its message boxes. NAC's own
script (NAC:ConfigScriptsLibrary on NAC_ConfigQuest) is called by name, so this compiles without NAC's sources. Global
functions, called by the client with the page's choices. docs/MENUS.md}

; ---------------------------------------------------------------- NAC's forms (NAC.esp)

string Function NacPlugin() global
	return "NAC.esp"
EndFunction

GlobalVariable Function Nac(int aiLocalID) global
	return Game.GetFormFromFile(aiLocalID, NacPlugin()) as GlobalVariable
EndFunction

; NAC_ConfigQuest's NAC:ConfigScriptsLibrary
Function CallLibrary(string asFunction, Var[] akArgs = None) global
	Quest config = Game.GetFormFromFile(0x010338, NacPlugin()) as Quest
	ScriptObject library = None
	if config
		library = config.CastAs("NAC:ConfigScriptsLibrary")
	endif
	if !library
		Debug.Trace("[RD] World settings: NAC's config library isn't loaded, " + asFunction + " not called", 1)
		return
	endif
	if !akArgs
		akArgs = new Var[0]
	endif
	library.CallFunction(asFunction, akArgs)
EndFunction

; ---------------------------------------------------------------- the holotape's refresh

; NAC:MFXConfigSettings.WaitForRefresh without its "please wait" box: the game hour NAC keeps, a moment (the camera
; switched to third person, first and back for the camera's settings), then the climate
Function Refresh(bool abUpdateCam = false) global
	CallLibrary("setGameHour")
	if abUpdateCam
		Game.ForceThirdPerson()
		Utility.Wait(0.5)
		Game.ForceFirstPerson()
		Utility.Wait(1.0)
		Game.ForceThirdPerson()
	else
		Utility.Wait(1.0)
	endif
	CallLibrary("RefreshClimate")
EndFunction

; ---------------------------------------------------------------- the camera

; The FOV override: 0 off, then 70, 80, 90, 100, 110, 120 as the holotape's buttons, each with its own third person and
; aiming FOV
Function SetFov(int aiOption) global
	int[] firstPerson = new int[7]
	int[] thirdPerson = new int[7]
	int[] aiming = new int[7]
	firstPerson[0] = 80
	thirdPerson[0] = 70
	aiming[0] = 65
	firstPerson[1] = 80
	thirdPerson[1] = 70
	aiming[1] = 60
	firstPerson[2] = 90
	thirdPerson[2] = 80
	aiming[2] = 65
	firstPerson[3] = 100
	thirdPerson[3] = 90
	aiming[3] = 70
	firstPerson[4] = 110
	thirdPerson[4] = 100
	aiming[4] = 75
	firstPerson[5] = 120
	thirdPerson[5] = 110
	aiming[5] = 80
	firstPerson[6] = 120
	thirdPerson[6] = 120
	aiming[6] = 85
	if aiOption < 0 || aiOption > 6
		return
	endif
	GlobalVariable useFov = Nac(0x023B83)
	useFov.SetValue(0.0)
	Refresh(false)
	useFov.SetValue(1.0)
	Nac(0x023B7A).SetValue(firstPerson[aiOption])
	Nac(0x023B7B).SetValue(thirdPerson[aiOption])
	Nac(0x023B7C).SetValue(aiming[aiOption])
	Refresh(true)
	if aiOption == 0
		; "Reset & Disable Override": the defaults set, then the override off
		useFov.SetValue(0.0)
		Refresh(false)
	endif
EndFunction

; The third person camera's position, NAC_UseCamIni (the page has written it): off first, then the new one
Function ApplyCameraPosition() global
	GlobalVariable position = Nac(0x027107)
	int chosen = position.GetValueInt()
	position.SetValue(0.0)
	Refresh(false)
	if chosen != 0
		position.SetValue(chosen)
		Refresh(true)
	endif
EndFunction

; Depth of field: aiMode 0 off, 1 static, 2 auto-focus; aiAperture 0 very small ... 4 very high (NAC's are 5 ... 1)
Function ApplyDof(int aiMode, int aiAperture) global
	int aperture = 5 - aiAperture
	if aperture < 1 || aperture > 5
		aperture = 2
	endif
	GlobalVariable autoFocus = Nac(0x025A29)
	GlobalVariable staticDof = Nac(0x024335)
	GlobalVariable autoAperture = Nac(0x0261D1)
	if aiMode == 1
		autoFocus.SetValue(0.0)
		staticDof.SetValue(aperture)
	elseif aiMode == 2
		staticDof.SetValue(0.0)
		autoFocus.SetValue(1.0)
		autoAperture.SetValue(aperture)
	else
		autoFocus.SetValue(0.0)
		staticDof.SetValue(0.0)
		autoAperture.SetValue(2.0)
	endif
	Refresh(false)
EndFunction

; ---------------------------------------------------------------- the look (NACFX)

; Every NACFX effect back to 0, as each effect's Reset
Function ResetLook() global
	Var[] reset = new Var[1]
	reset[0] = 0
	CallLibrary("setBrightness", reset)
	CallLibrary("setContrast", reset)
	CallLibrary("setHdrContrast", reset)
	CallLibrary("setSaturation", reset)
	CallLibrary("setGamma", reset)
	CallLibrary("setExposureMin", reset)
	CallLibrary("setSunlight", reset)
	CallLibrary("setSkylight", reset)
EndFunction

; ---------------------------------------------------------------- the weather (the host's)

; A weather of NAC's list (NAC_flNACWeathers) by its place there
Weather Function NacWeather(int aiIndex) global
	FormList weathers = Game.GetFormFromFile(0x00F3F9, NacPlugin()) as FormList
	if !weathers || aiIndex < 0 || aiIndex >= weathers.GetSize()
		return None
	endif
	return weathers.GetAt(aiIndex) as Weather
EndFunction

; Snow Now Overwhelms Winter's own weathers (Winter of Atom only), none of them in NAC's list: the page's numbers from
; 100 on (100 Blizzard, the prologue's; 101 Turbulent; 102 Medium; 103 Flurry; 104 Light)
Weather Function SnowWeather(int aiIndex) global
	int[] ids = new int[5]
	ids[0] = 0x005003 ; SNOW_Blizzard
	ids[1] = 0x005223 ; SNOW_SnowTurbulent
	ids[2] = 0x005333 ; SNOW_SnowMedium
	ids[3] = 0x005113 ; SNOW_SnowFlurry
	ids[4] = 0x005443 ; SNOW_SnowLite
	int i = aiIndex - 100
	if i < 0 || i >= ids.Length
		return None
	endif
	return Game.GetFormFromFile(ids[i], "SnowNowOverwhelmsWinter.esm") as Weather
EndFunction

; A weather the page lists: below 100 NAC's, from 100 on Snow Now Overwhelms Winter's
Weather Function PageWeather(int aiIndex) global
	if aiIndex >= 100
		return SnowWeather(aiIndex)
	endif
	return NacWeather(aiIndex)
EndFunction

; The holotape's "Load Selected Weather" (at once) and "Fade to Selected Weather". Reclamation Day sends the host's
; weather to everyone on the server.
Function LoadWeather(int aiIndex, bool abFade) global
	Weather chosen = PageWeather(aiIndex)
	if !chosen
		return
	endif
	Weather.ReleaseOverride()
	string how = "at once"
	if abFade
		how = "fading in"
		Weather current = Weather.GetCurrentWeather()
		if current
			current.ForceActive(false)
		endif
		chosen.SetActive(false, false)
	else
		chosen.ForceActive(false)
	endif
	Debug.Trace("[RD] World settings: weather " + chosen + " (the page's " + aiIndex + "), " + how)
EndFunction

; Winter of Atom's weathers (snow and clear ones, and radstorms, but no fire storm or time warp), their
; places in NAC's list: Clear Sky, Clear, Half Clear, Misty Snow, Foggy Snow, Snow, Snowstorm, Polluted, Radiated Fog,
; Acid Rain, Radstorm, Ultra Radstorm, Sulfur Storm, Fallout, Red Storm; then Snow Now Overwhelms Winter's five (the
; page's winter list)
Function LoadRandomWinterWeather() global
	int[] winter = new int[20]
	winter[0] = 1
	winter[1] = 0
	winter[2] = 30
	winter[3] = 41
	winter[4] = 35
	winter[5] = 36
	winter[6] = 37
	winter[7] = 32
	winter[8] = 3
	winter[9] = 24
	winter[10] = 4
	winter[11] = 34
	winter[12] = 38
	winter[13] = 33
	winter[14] = 26
	winter[15] = 100
	winter[16] = 101
	winter[17] = 102
	winter[18] = 103
	winter[19] = 104
	LoadWeather(winter[Utility.RandomInt(0, winter.Length - 1)], true)
EndFunction

; The holotape's "Ban Selected Weather From Current Climate" and "Reset Ban": NAC's blacklist and the weather's chance
Function BanWeather(int aiIndex, bool abBan) global
	Weather chosen = NacWeather(aiIndex)
	FormList blacklist = Game.GetFormFromFile(0x011A07, NacPlugin()) as FormList
	if !chosen || !blacklist
		return
	endif
	if abBan
		blacklist.AddForm(chosen)
		FormList chances = Game.GetFormFromFile(0x03C824, NacPlugin()) as FormList
		GlobalVariable chance = None
		if chances && aiIndex < chances.GetSize()
			chance = chances.GetAt(aiIndex) as GlobalVariable
		endif
		if chance
			chance.SetValue(0.0)
		endif
		if Weather.GetCurrentWeather() == chosen
			CallLibrary("LoadRandomWeather")
		endif
	else
		blacklist.RemoveAddedForm(chosen)
		CallLibrary("RefreshClimate")
	endif
EndFunction

; A kind of weather banned from the climate or allowed again (0 sunny, 1 overcast, 2 rain, 3 fog, 4 snow, 5 radstorms),
; as the holotape's "Ban All ... Weathers" and "Reset Ban for All ..."
Function BanWeatherGroup(int aiKind, bool abBan) global
	string[] packs = new string[6]
	packs[0] = "NAC_flwthrPleasant"
	packs[1] = "NAC_flwthrOvercast"
	packs[2] = "NAC_flwthrRain"
	packs[3] = "NAC_flwthrFog"
	packs[4] = "NAC_flwthrSnow"
	packs[5] = "NAC_flwthrRadstorms"
	if aiKind < 0 || aiKind >= packs.Length
		return
	endif
	Var[] args = new Var[2]
	args[0] = abBan
	args[1] = packs[aiKind]
	CallLibrary("BanPack", args)
EndFunction

; ---------------------------------------------------------------- reset

; The holotape's "Reset to NAC Default" and "Disable ALL" for the look, the camera and the effects. The world's settings
; (timescale, radstorms everywhere) are the host's, on the Weather tab, and stay as they are.
Function ResetNac(bool abAllOff) global
	float on = 1.0
	if abAllOff
		on = 0.0
	endif
	Nac(0x01D87C).SetValue(0.0)                 ; NAC_LumNight
	Nac(0x024332).SetValue(0.0)                 ; NAC_LumInt
	Nac(0x01D876).SetValue(on)                  ; NAC_UseAdaptation
	Nac(0x01B261).SetValue(on)                  ; NAC_UseChemFX
	Nac(0x027107).SetValue(0.0)                 ; NAC_UseCamIni
	Nac(0x01387F).SetValue(on)                  ; NAC_UseCloudShadows
	Nac(0x024335).SetValue(0.0)                 ; NAC_UseDOFStatic
	Nac(0x025A29).SetValue(0.0)                 ; NAC_UseDOFAutoFocus
	Nac(0x01E018).SetValue(0.0)                 ; NAC_UseENB
	Nac(0x018C40).SetValue(on)                  ; NAC_UseFakeInt
	Nac(0x023B83).SetValue(0.0)                 ; NAC_UseFOV
	Nac(0x023B7A).SetValue(80.0)                ; NAC_FOV1p
	Nac(0x023B7B).SetValue(70.0)                ; NAC_FOV3p
	Nac(0x023B7C).SetValue(65.0)                ; NAC_FOVaim
	Nac(0x012948).SetValue(on)                  ; NAC_UseHitFX
	Nac(0x0147B4).SetValue(on)                  ; NAC_UseIntOvr
	Nac(0x00FB9D).SetValue(on)                  ; NAC_UseLensDist
	Nac(0x00FB9B).SetValue(2.0 * on)            ; NAC_UseNoise
	Nac(0x01B260).SetValue(on)                  ; NAC_UseRadFX
	Nac(0x018C44).SetValue(on)                  ; NAC_UseWeatherFX
	Nac(0x018C32).SetValue(on)                  ; NAC_UseSunG
	Nac(0x00FB9C).SetValue(2.0 * on)            ; NAC_UseVign
	Nac(0x022C37).SetValue(1.0)                 ; NAC_UseVL (on in both)
	Nac(0x051751).SetValue(on)                  ; NAC_UseRadSiren
	Refresh(true)
EndFunction
