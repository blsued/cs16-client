
## V5 CAPTURE RUN started 2026-06-18T02:30:14.8229832-06:00 (CAPTURE subagent)
2026-06-18T02:30:48.4757196-06:00 - dll tokens verified (csz_debugcam, active pos=, water_draw present). Acquiring lock.
2026-06-18T02:30:48.6594044-06:00 - lock acquire exit=0
2026-06-18T02:30:57.2306052-06:00 - STEP1 smoke run: water_on_de_aztec windowed RunId=v5smoke1
powershell.exe : D:\csoz-wt\weather\R2_CAPTURE\capture_r2_weather.ps1 : Cannot process argument transformation on 
parameter 
At line:3 char:8
+ $out = & powershell -NoProfile -ExecutionPolicy Bypass -File 'D:\csoz ...
+        ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    + CategoryInfo          : NotSpecified: (D:\csoz-wt\weat...n on parameter :String) [], RemoteException
    + FullyQualifiedErrorId : NativeCommandError
 
'AcquireLock'. Cannot convert value "System.String" to type "System.Boolean". Boolean parameters accept only Boolean 
values and numbers, such as $True, $False, 1 or 0.
    + CategoryInfo          : InvalidData: (:) [capture_r2_weather.ps1], ParentContainsErrorRecordException
    + FullyQualifiedErrorId : ParameterArgumentTransformationError,capture_r2_weather.ps1
 

2026-06-18T02:31:05.3949971-06:00 STEP1 OUTPUT:
powershell.exe : D:\csoz-wt\weather\R2_CAPTURE\capture_r2_weather.ps1 : Cannot process argument transformation on 
parameter 
At line:2 char:8
+ $out = & powershell -NoProfile -ExecutionPolicy Bypass -File 'D:\csoz ...
+        ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    + CategoryInfo          : NotSpecified: (D:\csoz-wt\weat...n on parameter :String) [], RemoteException
    + FullyQualifiedErrorId : NativeCommandError
 
'AcquireLock'. Cannot convert value "System.String" to type "System.Boolean". Boolean parameters accept only Boolean 
values and numbers, such as $True, $False, 1 or 0.
    + CategoryInfo          : InvalidData: (:) [capture_r2_weather.ps1], ParentContainsErrorRecordException
    + FullyQualifiedErrorId : ParameterArgumentTransformationError,capture_r2_weather.ps1
 

2026-06-18T02:31:22.8121426-06:00 STEP1 OUTPUT:

2026-06-18T02:35:46.8944644-06:00 - FINAL full sweep RunId=v5final (all 6, poses locked)

2026-06-18T02:39:22.3945552-06:00 - lock released exit=0. V5 capture complete: 6/6 deliverables, worstcase gate PASS (avg404 1low396 68smp), cap defeated.
# R2 Capture V5 Log (RunId v5bright)

## Thu Jun 18 02:52:00 MDT 2026
- DLL SHA confirmed: deployed == built == 0ADE1E36ED483D646E847952C2DDCFB45819E9542DEDEA243399B1DBAA247B83 (MATCH)
- Starting capture with -RunId v5bright -SkyPhase 0.95

## RECAPTURE v5bright2 (2026-06-18 02:55:37)
- LOCK acquired: weather -> D:\csoz\_orch\engine-capture.lock
- Deployed DLL SHA256 = 0ADE1E36ED483D646E847952C2DDCFB45819E9542DEDEA243399B1DBAA247B83 (== expected, CONFIRMED)
- Build DLL SHA256 matches deployed.
- Running FULL capture FOREGROUND: -RunId v5bright2 -SkyPhase 0.95
