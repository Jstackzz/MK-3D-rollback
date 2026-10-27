<#
.SYNOPSIS
  End-to-end online test on one PC: the lobby server, two launchers (separate settings, game ports Port+10/+11)
  that sign in, queue for a casual match of the chosen game and mode and auto-accept, and the two real game instances
  they start. Both games get scripted input (<GAME>_INPUT) so the match ends. Prints every status line of both games,
  then the server's view of the two accounts. -Queue lobby -Watch adds a third launcher, C, that joins the lobby as a
  spectator (-WatchDelay seconds after B): its game plays the lobby's matches from the players' feeds, and the run
  compares its per-frame log with A's (tools\py\rbcmp.py).

.EXAMPLE
  powershell -File tools\online-e2e.ps1 -Seconds 240
  powershell -File tools\online-e2e.ps1 -Game mka -Seconds 300
#>
param(
    [int]$Seconds = 240,
    [string]$Game = 'mkd',       # mkd, mka, mkda
    [string]$Exe = '',           # default: recomp\<game>\out\build\debug\<GAME>.exe, or the -Launcher package's game
    [string]$Launcher = '',      # default: build\launcher\MK3DLauncher.exe; a player package's launcher uses its own games
    [string]$Queue = 'casual',   # casual, ranked, or lobby (A hosts a lobby, B joins it, the host starts)
    [string]$Mode = 'kombat',    # kombat, chess, puzzle (mkd), motor (mka)
    [string]$CharA = 'Scorpion',
    [string]$CharB = 'Kobra',
    [string]$InputA = '',        # <GAME>_INPUT of each game (default: the same scripted attacker for both)
    [string]$InputB = '',
    [int]$Matches = 1,           # lobby: matches to play there one after the other (the games park and start again)
    [int]$Rematches = 0,         # casual: rematches both choose after the first match (a ranked set plays on by
                                 # itself until one player has won it)
    [string[]]$RematchChoice = @(),  # casual: what both choose each time, rematch (the same fighters) or select (both
                                 # pick again), e.g. select,rematch, the last one repeating; default rematch
    [switch]$Relay,              # the server relays every match (--relay-only): the relay path, on one PC
    [switch]$StandbyA,           # A plays single player while it searches (--solo): the match freezes it, then A
                                 # leaves the series (--leave-series) and single player runs on
    [switch]$PracticeA,          # A practises while it searches (practice while you wait): the match freezes the
                                 # practice, A leaves the series after it and practice runs on
    [int]$DelayB = 0,            # seconds before B starts (and queues): time for A's practice to come up
    [int]$Pads = 2,              # controllers each game reports at least (<GAME>_PADS); 0: only what is plugged in, as a player's PC
    [switch]$PickInGame,         # both players pick on the game's own select screen (their scripts move the cursor
                                 # and press A first); otherwise the launchers pick CharA and CharB
    [switch]$MixedPicks,         # with -PickInGame: only A picks in the game, B's launcher picks CharB
    [switch]$Watch,              # lobby: C watches it (a spectator)
    [int]$WatchDelay = 0,        # seconds after B before C starts (a spectator who comes in late)
    [string]$DeathTraps = '',    # on / off: the death traps the host's lobby (or A's search) asks for
    [int]$Port = 7777            # the server's TCP port (UDP Port + 1), the games' Port + 10 and + 11; the work folder
                                 # is build\online-e2e (build\online-e2e-<Port> for another port), so runs side by side
                                 # need their own
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($PickInGame) { $CharA = '-'; if (-not $MixedPicks) { $CharB = '-' } }  # lobby members pick in the game too
$x = $Game.ToUpper()  # each game reads its own variables: MKD_*, MKA_*, MKDA_*
if (-not $Exe -and -not $Launcher) { $Exe = "recomp\$Game\out\build\debug\$x.exe" }
$work = Join-Path $root ($(if ($Port -eq 7777) { 'build\online-e2e' } else { "build\online-e2e-$Port" }))
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$work\a", "$work\b", "$work\c", "$work\server" | Out-Null
Copy-Item (Join-Path $root 'build\server-dev\server-cert.pem'), (Join-Path $root 'build\server-dev\server-key.pem') "$work\server\"
$fp = (Get-Content (Join-Path $root 'build\launcher-dev\mk3d-launcher.ini') | Where-Object { $_ -like 'fingerprint=*' }) -replace '^fingerprint=', ''
# a package's launcher finds its games itself; any other gets the executable
$exePath = if ([IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $root $Exe }
$gamesIni = if ($Exe) { "[games]`r`n$Game\exe=$($exePath -replace '\\', '/')" } else { '' }
foreach ($p in @(@('a', ($Port + 10)), @('b', ($Port + 11)), @('c', ($Port + 12)))) {
    $ini = @"
[server]
address=127.0.0.1:$Port
fingerprint=$fp
auto_connect=true

$gamesIni

[online]
region=eu
max_ping=0
delay=2
game_port=$($p[1])
pick_in_game=$(if ($PickInGame -and ($p[0] -eq 'a' -or -not $MixedPicks)) { 'true' } else { 'false' })
practice_while_waiting=$(if ($PracticeA -and $p[0] -eq 'a') { 'true' } else { 'false' })

[account]
remember=false

[display]
mode=windowed
resolution=960x720

[dev]
udp_loopback=true
"@
    Set-Content -Path "$work\$($p[0])\mk3d-launcher.ini" -Value $ini -Encoding ascii
}
$py = (Get-Command python).Source
$serverArgs = @('-m', 'server', '--host', '127.0.0.1', '--port', "$Port", '--udp-port', "$($Port + 1)", '--db', "`"$work\server\e2e.sqlite`"")
if ($Queue -eq 'ranked') { $serverArgs += '--allow-same-ip' }  # ranked never pairs one public IP otherwise
if ($Relay) { $serverArgs += '--relay-only' }
$server = Start-Process -FilePath $py -ArgumentList $serverArgs `
    -WorkingDirectory $root -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\server\out.txt" -RedirectStandardError "$work\server\err.txt"
Start-Sleep -Seconds 2
$attacks = & $py -I -c "import itertools; seq=['RIGHT:0.4','X','X','Y','A','B','X','Y','X+Y','LEFT:0.3','B','A']; print(','.join(f'{25+i*0.33:.2f}:{s}' for i,s in enumerate(itertools.islice(itertools.cycle(seq),2500))))"
# picking in the game the script starts when the player gets the cursor: each moves theirs, then confirms
$pickA = if ($PickInGame) { '1.0:RIGHT,1.6:RIGHT,2.4:A,' } else { '' }
$pickB = if ($PickInGame -and -not $MixedPicks) { '1.2:DOWN,2.0:LEFT,2.8:A,3.6:A,' } else { '' }
if (-not $InputA) { $InputA = $pickA + $attacks }
if (-not $InputB) { $InputB = $pickB + $attacks }
$vars = @('MK3D_DEV_PASSWORD', "${x}_INPUT", "${x}_PADS", "${x}_EVENT_LOG", "${x}_NET_LOG", 'SDL_ASSERT')
$env:MK3D_DEV_PASSWORD = 'e2e-password-123'
Set-Item "env:${x}_INPUT" $InputA
Set-Item "env:${x}_PADS" "$Pads"
Set-Item "env:${x}_EVENT_LOG" '1'
Set-Item "env:${x}_NET_LOG" "$work\net-{side}-{match}.log"
$env:SDL_ASSERT = 'always_ignore'
$launcher = if ($Launcher) { (Resolve-Path $Launcher).Path } else { Join-Path $root 'build\launcher\MK3DLauncher.exe' }
$stamp = Get-Date -Format 'HHmmss'
if ($Queue -eq 'casual' -and $Rematches -gt 0) { $Matches = 1 + $Rematches }
$queueArgs = @('--queue', $Queue, '--rematches', "$Rematches")
if ($RematchChoice) { $queueArgs += @('--rematch-choice', ($RematchChoice -join ',')) }  # one argument, no spaces
$how = if ($Queue -eq 'lobby') { @('--lobby', 'host', '--lobby-name', "e2e$stamp", '--lobby-matches', "$Matches") } else { $queueArgs }
if ($StandbyA) { $how += @('--solo', '--leave-series') }
if ($DeathTraps) { $how += @('--death-traps', $DeathTraps) }
if ($PracticeA) { $how += @('--leave-series') }
$la = Start-Process -FilePath $launcher -ArgumentList (@('--config', "`"$work\a\mk3d-launcher.ini`"", '--user', "e2eA$stamp", '--register') + $how + @('--game', $Game, '--mode', $Mode, '--character', $CharA, '--auto-accept', '3')) -PassThru
Start-Sleep -Seconds ([Math]::Max(1, $DelayB))
Set-Item "env:${x}_INPUT" $InputB
$how = if ($Queue -eq 'lobby') { @('--lobby', 'join', '--lobby-name', "e2e$stamp", '--lobby-matches', "$Matches") } else { $queueArgs }
$lb = Start-Process -FilePath $launcher -ArgumentList (@('--config', "`"$work\b\mk3d-launcher.ini`"", '--user', "e2eB$stamp", '--register') + $how + @('--game', $Game, '--mode', $Mode, '--character', $CharB, '--auto-accept', '3')) -PassThru
$lc = $null
if ($Watch) {
    Start-Sleep -Seconds ([Math]::Max(1, $WatchDelay))
    Remove-Item "env:${x}_INPUT" -ErrorAction SilentlyContinue  # a spectator's game takes no player's input
    $lc = Start-Process -FilePath $launcher -ArgumentList (@('--config', "`"$work\c\mk3d-launcher.ini`"", '--user', "e2eC$stamp", '--register', '--lobby', 'watch', '--lobby-name', "e2e$stamp", '--game', $Game, '--mode', $Mode)) -PassThru
}
foreach ($k in $vars) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
"server pid $($server.Id), launchers $($la.Id) $($lb.Id)$(if ($lc) { " $($lc.Id)" }), accounts e2eA$stamp / e2eB$stamp$(if ($lc) { " / e2eC$stamp (watching)" })"
$t0 = Get-Date
$seen = @{}
$results = 0
$watched = 0
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    foreach ($s in Get-ChildItem -Path "$work\a\sessions", "$work\b\sessions", "$work\c\sessions" -Directory -ErrorAction SilentlyContinue) {
        $f = Join-Path $s.FullName 'status.txt'
        if (-not (Test-Path $f)) { continue }
        $line = (Get-Content $f -TotalCount 1 -ErrorAction SilentlyContinue)
        $key = "$($s.Parent.Parent.Name)/$($s.Name)"
        if ($line -and $seen[$key] -ne $line) {
            $seen[$key] = $line
            "{0,6:N1}s {1}: {2}" -f ((Get-Date) - $t0).TotalSeconds, $key, $line
            if ($line -like 'result*' -and $key -like 'c/*') { $watched++ }
            elseif ($line -like 'result*') { $results++ }
        }
    }
    # the server's RESULT lines count the matches done (a game's result status can last less than a poll); a ranked
    # set is done when the server closes it
    $serverLog = @(Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue)
    $done = @($serverLog | Select-String -Pattern 'RESULT match=').Count
    $finished = if ($Queue -eq 'ranked') { @($serverLog | Select-String -Pattern 'SET \S+ over').Count -ge 1 }
                else { $results -ge 2 * $Matches -or $done -ge $Matches }
    if ($Watch) { $finished = $finished -and $watched -ge $Matches }  # the spectator runs behind the players
    if ($finished) { Start-Sleep -Seconds 12; break }
    Start-Sleep -Milliseconds 250
}
$who = @('a', 'b') + $(if ($Watch) { @('c') } else { @() })
"--- launcher logs (last lines)"
foreach ($s in $who) { Get-Content "$work\$s\mk3d-launcher.log" -Tail 8 -ErrorAction SilentlyContinue | ForEach-Object { "[$s] $_" } }
"--- match starts (the fighters each game was given; empty: picked on the select screen)"
foreach ($s in $who) {
    Get-ChildItem -Path "$work\$s\sessions" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        # a game older than its session log has none (Select-String stops the script on a missing path)
        $log = Join-Path $_.FullName 'game.log'
        if (Test-Path $log) {
            Select-String -Path $log -Pattern 'link: start|picked character|death traps|watching|DESYNC|leaving the match' | ForEach-Object { "[$s] $($_.Line)" }
        }
    }
}
"--- server log (matches and results)"
Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'match|result|elo|report|set |series' | Select-Object -Last 16 | ForEach-Object { $_.Line }
# only what this run started: the two launchers and the games they started
$ls = @($la, $lb) + @($lc | Where-Object { $_ })
$games = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -in @($ls | ForEach-Object { $_.Id }) } | ForEach-Object { $_.ProcessId })
foreach ($p in $ls) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 4
foreach ($id in @($ls | ForEach-Object { $_.Id }) + $games) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
if ($Watch) {
    "--- the spectator's frames against A's (the last run of every frame, the games closed)"
    foreach ($w in Get-ChildItem -Path $work -Filter 'net-w-*.log' -ErrorAction SilentlyContinue) {
        $a = Join-Path $work ($w.Name -replace '^net-w-', 'net-0-')
        if (Test-Path $a) { & $py -I (Join-Path $root 'tools\py\rbcmp.py') $a $w.FullName | Select-Object -First 3 }
    }
}
"done"
